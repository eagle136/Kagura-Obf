#include "kagura/Options.h"
#include "kagura/Utils.h"
#include "llvm/ADT/StringRef.h"

#if __has_include("llvm/TargetParser/Triple.h")
#include "llvm/TargetParser/Triple.h"  // LLVM 20+
#else
#include "llvm/ADT/Triple.h"           // LLVM 17-19
#endif
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <random>
#include <regex>

using namespace llvm;

namespace kagura {

// ---- Annotation helpers ----

bool hasAnnotation(Function &F, StringRef Attr) {
  Module *M = F.getParent();
  GlobalVariable *GA =
      M->getGlobalVariable("llvm.global.annotations");
  if (!GA)
    return false;
  auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
  if (!CA)
    return false;
  for (unsigned I = 0; I < CA->getNumOperands(); ++I) {
    auto *CS = dyn_cast<ConstantStruct>(CA->getOperand(I));
    if (!CS || CS->getNumOperands() < 2)
      continue;
    auto *FPtr =
        dyn_cast<Function>(CS->getOperand(0)->stripPointerCasts());
    if (FPtr != &F)
      continue;
    auto *GV =
        dyn_cast<GlobalVariable>(CS->getOperand(1)->stripPointerCasts());
    if (!GV || !GV->hasInitializer())
      continue;
    auto *Str = dyn_cast<ConstantDataArray>(GV->getInitializer());
    if (!Str)
      continue;
    // 2026-09-25 (audit P2-2): token-exact match instead of substring
    // matching.  Substring matching made annotate("kagura_peephole_hint")
    // hit "kagura_pe" and annotate("kagura_nostring_x") hit "kagura_nostr".
    StringRef            AttrStr = Str->getAsCString();
    SmallVector<StringRef, 8> Tokens;
    AttrStr.split(Tokens, ", \t", /*MaxSplit=*/-1, /*KeepEmpty=*/false);
    for (StringRef Tok : Tokens)
      if (Tok.trim() == Attr)
        return true;
  }
  return false;
}

// ---- List-matching helpers (4.6.3 / 4.6.4) --------------------------------

/// Returns true if Name matches any pattern in the comma-separated list.
/// Patterns may use a trailing '*' glob (e.g. "my_prefix_*").
static bool matchesList(StringRef Name, StringRef List) {
  if (List.empty())
    return false;
  SmallVector<StringRef, 16> Patterns;
  List.split(Patterns, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  for (StringRef Pat : Patterns) {
    Pat = Pat.trim();
    if (Pat.empty())
      continue;
    if (Pat.ends_with("*")) {
      if (Name.starts_with(Pat.drop_back(1)))
        return true;
    } else {
      if (Name == Pat)
        return true;
    }
  }
  return false;
}

// ---- Static-link / LTO function filtering (4.6.5) --------------------------

/// Returns true if F looks like a compiler-rt / runtime library function that
/// should NOT be obfuscated during static linking or LTO.  These functions
/// come from precompiled bitcode or library archives and obfuscating them
/// can cause infinite expansion, OOM, or compile hangs.
///
/// The heuristic is based on the function name alone (no Module metadata)
/// because during LTO the linkage/section info may not be reliable.
static bool isSystemRuntimeFunction(StringRef Name) {
  // --- Standard intrinsic / builtin prefixes ---

  // LLVM intrinsic functions
  if (Name.starts_with("llvm."))
    return true;

  // compiler-rt / libgcc builtins: __udivmoddi4, __chkstk, __muloti4, ...
  if (Name.starts_with("__"))
    return true;

  // GNU runtime helpers injected by the compiler
  if (Name.starts_with("__gnu_"))
    return true;

  // C++ ABI helpers (itanium / MSVC)
  if (Name.starts_with("__cxa") || Name.starts_with("__cxx") ||
      Name.starts_with("_Z") || Name.starts_with("_GLOBAL_") ||
      Name.starts_with("_fini_") || Name.starts_with("_init_") ||
      Name.starts_with(".L") || Name.starts_with(".l_"))
    return true;

  // Microsoft CRT symbols
  if (Name.starts_with("_except_") || Name.starts_with("_chkstk") ||
      Name.starts_with("_ftol") || Name.starts_with("_ltoa") ||
      Name.starts_with("_ultoa") || Name.starts_with("_gcvt") ||
      Name.starts_with("_mb_") || Name.starts_with("_isctype") ||
      Name.starts_with("_seh_") || Name.starts_with("__C_specific_handler") ||
      Name.starts_with("_guard_"))
    return true;

  // Sanitizer runtime functions (if not caught by attribute check above)
  if (Name.starts_with("asan_") || Name.starts_with("__sanitizer_") ||
      Name.starts_with("tsan_") || Name.starts_with("ubsan_") ||
      Name.starts_with("msan_") || Name.starts_with("hwasan_"))
    return true;

  // Common libc / compiler-rt function names that appear as bitcode
  // during static linking (memcpy, memset, etc. are often LLVM intrinsics
  // but their wrapper implementations may appear as defined functions)
  if (Name == "memcpy" || Name == "memset" || Name == "memmove" ||
      Name == "memcmp" || Name == "strlen" || Name == "strcmp" ||
      Name == "bcmp" || Name == "abort" || Name == "longjmp" ||
      Name == "setjmp" || Name == "alloca" || Name == "printf" ||
      Name == "puts" || Name == "fprintf" || Name == "sprintf" ||
      Name == "snprintf" || Name == "scanf" || Name == "fscanf" ||
      Name == "sscanf" || Name == "malloc" || Name == "free" ||
      Name == "calloc" || Name == "realloc" || Name == "exit" ||
      Name == "atexit" || Name == "fopen" || Name == "fclose" ||
      Name == "fread" || Name == "fwrite" || Name == "fseek" ||
      Name == "ftell" || Name == "rewind" || Name == "fflush" ||
      Name == "setbuf" || Name == "setvbuf" ||
      // ---- POSIX / NDK extended (4.6.6) ----
      // string.h extensions
      Name == "strcpy" || Name == "strncpy" || Name == "strcat" ||
      Name == "strncat" || Name == "strchr" || Name == "strrchr" ||
      Name == "strstr" || Name == "strtok" || Name == "strtok_r" ||
      Name == "strspn" || Name == "strcspn" || Name == "strpbrk" ||
      Name == "strerror" || Name == "strftime" || Name == "strtol" ||
      Name == "strtoll" || Name == "strtoul" || Name == "strtoull" ||
      Name == "strtof" || Name == "strtod" || Name == "memchr" ||
      Name == "memrchr" ||
      // stdlib.h extensions
      Name == "atoi" || Name == "atol" || Name == "atoll" ||
      Name == "rand" || Name == "srand" || Name == "abs" ||
      Name == "labs" || Name == "llabs" || Name == "div" ||
      Name == "ldiv" || Name == "lldiv" || Name == "qsort" ||
      Name == "bsearch" || Name == "getenv" || Name == "system" ||
      Name == "mblen" || Name == "mbtowc" || Name == "wctomb" ||
      Name == "mbstowcs" || Name == "wcstombs" ||
      // stdio.h extensions
      Name == "perror" || Name == "popen" || Name == "pclose" ||
      Name == "tmpfile" || Name == "tmpnam" || Name == "remove" ||
      Name == "rename"  ||
      // unistd.h
      Name == "open" || Name == "close" || Name == "read" ||
      Name == "write" || Name == "lseek" || Name == "ftruncate" ||
      Name == "truncate" || Name == "access" || Name == "unlink" ||
      Name == "link" || Name == "symlink" || Name == "readlink" ||
      Name == "chdir" || Name == "getcwd" || Name == "rmdir" ||
      Name == "mkdir" || Name == "sync" || Name == "fsync" ||
      Name == "dup" || Name == "dup2" || Name == "pipe" ||
      Name == "sleep" || Name == "usleep" || Name == "getpid" ||
      Name == "getppid" || Name == "getuid" || Name == "geteuid" ||
      Name == "getgid" || Name == "getegid" || Name == "sysconf" ||
      Name == "pathconf" || Name == "gethostname" || Name == "sethostname" ||
      Name == "fcntl" || Name == "creat" || Name == "openat" ||
      // math.h
      Name == "fabsf" || Name == "fabs" || Name == "sqrtf" ||
      Name == "sqrt" || Name == "sinf" || Name == "sin" ||
      Name == "cosf" || Name == "cos" || Name == "ceilf" ||
      Name == "ceil" || Name == "floorf" || Name == "floor" ||
      Name == "roundf" || Name == "round" || Name == "truncf" ||
      Name == "trunc" || Name == "powf" || Name == "pow" ||
      Name == "logf" || Name == "log" || Name == "expf" ||
      Name == "exp" || Name == "tanf" || Name == "tan" ||
      Name == "atanf" || Name == "atan" || Name == "atan2f" ||
      Name == "atan2" || Name == "asin" || Name == "acos" ||
      Name == "fmodf" || Name == "fmod" || Name == "hypotf" ||
      Name == "hypot" || Name == "sinhf" || Name == "sinh" ||
      Name == "coshf" || Name == "cosh" || Name == "tanhf" ||
      Name == "tanh" || Name == "log2f" || Name == "log2" ||
      Name == "log10f" || Name == "log10" || Name == "exp2f" ||
      Name == "exp2" || Name == "cbrtf" || Name == "cbrt" ||
      Name == "erff" || Name == "erf" || Name == "copysignf" ||
      Name == "copysign" || Name == "fdimf" || Name == "fdim" ||
      Name == "fmaxf" || Name == "fmax" || Name == "fminf" ||
      Name == "fmin" || Name == "remainderf" || Name == "remainder" ||
      Name == "rintf" || Name == "rint" || Name == "nearbyintf" ||
      Name == "nearbyint" || Name == "scalbnf" || Name == "scalbn" ||
      // dirent.h
      Name == "opendir" || Name == "readdir" || Name == "readdir_r" ||
      Name == "closedir" || Name == "rewinddir" || Name == "telldir" ||
      Name == "seekdir" || Name == "scandir" || Name == "alphasort" ||
      Name == "versionsort" ||
      // mntent.h
      Name == "setmntent" || Name == "getmntent" || Name == "endmntent" ||
      Name == "hasmntopt" || Name == "addmntent" ||
      // sys/mount.h
      Name == "mount" || Name == "umount" || Name == "umount2" ||
      // sys/ioctl.h
      Name == "ioctl" ||
      // sys/stat.h
      Name == "stat" || Name == "lstat" || Name == "fstat" ||
      Name == "mknod" || Name == "chmod" || Name == "fchmod" ||
      Name == "umask" || Name == "chown" || Name == "fchown" ||
      Name == "lchown" || Name == "mknodat" || Name == "mkfifo" ||
      // signal.h
      Name == "signal" || Name == "sigaction" || Name == "sigprocmask" ||
      Name == "kill" || Name == "raise" || Name == "sigemptyset" ||
      Name == "sigfillset" || Name == "sigaddset" || Name == "sigdelset" ||
      Name == "sigismember" ||
      // time.h
      Name == "time" || Name == "localtime" || Name == "gmtime" ||
      Name == "mktime" || Name == "asctime" || Name == "ctime" ||
      Name == "difftime" || Name == "clock_gettime" ||
      Name == "clock_settime" || Name == "clock_getres" ||
      Name == "nanosleep" || Name == "timespec_get" ||
      // ctype.h
      Name == "isalnum" || Name == "isalpha" || Name == "isblank" ||
      Name == "iscntrl" || Name == "isdigit" || Name == "isgraph" ||
      Name == "islower" || Name == "isprint" || Name == "ispunct" ||
      Name == "isspace" || Name == "isupper" || Name == "isxdigit" ||
      Name == "tolower" || Name == "toupper" ||
      // dlfcn.h
      Name == "dlopen" || Name == "dlclose" || Name == "dlsym" ||
      Name == "dlerror" ||
      // glob.h
      Name == "glob" || Name == "globfree" ||
      // errno.h
      Name == "strerror" || Name == "perror" ||
      // stdatomic.h / threads.h (C11)
      Name == "thrd_create" || Name == "thrd_join" || Name == "thrd_exit" ||
      Name == "thrd_sleep" || Name == "thrd_yield" ||
      Name == "mtx_init" || Name == "mtx_lock" ||
      Name == "mtx_unlock" || Name == "mtx_destroy" ||
      Name == "cnd_init" || Name == "cnd_signal" ||
      Name == "cnd_broadcast" || Name == "cnd_wait" ||
      Name == "cnd_destroy" ||
      // sys/socket.h / netdb.h (basic networking)
      Name == "socket" || Name == "bind" || Name == "listen" ||
      Name == "accept" || Name == "connect" || Name == "send" ||
      Name == "recv" || Name == "sendto" || Name == "recvfrom" ||
      Name == "setsockopt" || Name == "getsockopt" || Name == "shutdown" ||
      Name == "gethostbyname" || Name == "getaddrinfo" ||
      Name == "freeaddrinfo" || Name == "htons" || Name == "htonl" ||
      Name == "ntohs" || Name == "ntohl" ||
      // sys/wait.h / sys/resource.h
      Name == "wait" || Name == "waitpid" || Name == "waitid" ||
      Name == "getpriority" || Name == "setpriority" ||
      Name == "getrlimit" || Name == "setrlimit" ||
      // sys/time.h
      Name == "gettimeofday" || Name == "settimeofday" ||
      // ucontext.h
      Name == "getcontext" || Name == "setcontext" ||
      Name == "makecontext" || Name == "swapcontext")
    return true;

  // Prefix-based matching for families with many member functions
  if (Name.starts_with("pthread_") || Name.starts_with("sem_") ||
      Name.starts_with("dl_") || Name.starts_with("sched_")  ||
      Name.starts_with("cpu_") || Name.starts_with("gettid") ||
      Name.starts_with("epoll_") || Name.starts_with("poll_") ||
      Name.starts_with("prctl") || Name.starts_with("ptrace") ||
      Name.starts_with("sched_") || Name.starts_with("utmpx") ||
      Name.starts_with("syscall"))
    return true;

  return false;
}

// ---- Skip pattern (regex) cache ----

/// Compile and cache the regex from -kagura-skip-pattern.  The pattern is
/// compiled once on first access to avoid O(n) recompilation per function.
static const std::regex &getSkipPattern() {
  static std::regex *Cached = nullptr;
  if (!Cached) {
    if (opt::SkipPattern.empty()) {
      static std::regex Empty("");
      Cached = &Empty;
    } else {
#if defined(__cpp_exceptions)
      try {
        Cached = new std::regex(opt::SkipPattern.getValue(),
                                std::regex::ECMAScript);
      } catch (const std::regex_error &) {
        // If the user provides an invalid regex, silently fall through
        // (no filtering) rather than crash.
        static std::regex Empty("");
        Cached = &Empty;
      }
#else
      // LLVM pass plugins build with -fno-exceptions (no try/catch allowed).
      // Construct directly; an invalid pattern will abort opt — acceptable
      // because the default (empty) pattern never reaches this branch.
      Cached = new std::regex(opt::SkipPattern.getValue(),
                              std::regex::ECMAScript);
#endif
    }
  }
  return *Cached;
}

bool shouldObfuscate(Function &F, StringRef PassAttr, bool GlobalFlag) {
  // Never obfuscate kagura's own injected helper functions
  if (F.getName().starts_with("kagura_"))
    return false;

  // 4.1.9: Sanitizer compatibility — skip obfuscation when ASan, TSan, UBSan,
  // or MSan function attributes are present to prevent false positives.
  // Functions built with sanitizers have __sanitizer_* attributes or
  // the "noinstrument" attribute set by the sanitizer runtime.
  if (F.hasFnAttribute(Attribute::SanitizeAddress) ||
      F.hasFnAttribute(Attribute::SanitizeThread)  ||
      F.hasFnAttribute(Attribute::SanitizeMemory)  ||
      F.hasFnAttribute(Attribute::SanitizeHWAddress))
    return false;

  StringRef Name = F.getName();

  // 4.6.5: Auto-detect system/runtime functions in static-link/LTO builds.
  // These checks run before the force-protect list so that system functions
  // are skipped *even if* the user explicitly listed them in -kagura-protect
  // (obfuscating compiler-rt builtins is unsafe and will cause hangs).
  if (isSystemRuntimeFunction(Name))
    return false;

  // 4.6.5: User-supplied regex pattern for fine-grained skipping.
  if (!opt::SkipPattern.empty()) {
    const std::regex &Pat = getSkipPattern();
    if (std::regex_match(Name.begin(), Name.end(), Pat))
      return false;
  }

  // 4.6.4: denylist — explicit exclusion takes highest precedence
  // 2026-09-25 (audit P2-5): checked before the protect list; previously
  // protect returned first, silently disabling the deny list.
  if (!opt::DenyList.empty() && matchesList(Name, opt::DenyList))
    return false;

  // 4.6.3: force-protect list — overrides everything else below
  if (!opt::ProtectList.empty() && matchesList(Name, opt::ProtectList))
    return true;

  // 4.8.2: hot path annotation — skip obfuscation on performance-critical funcs
  if (hasAnnotation(F, "kagura_hotpath"))
    return false;

  // 4.6.4: allowlist mode — when set, only matching symbols are obfuscated
  if (!opt::AllowList.empty() && !matchesList(Name, opt::AllowList))
    return false;

  std::string EnableAttr  = ("kagura_" + PassAttr).str();
  std::string DisableAttr = ("kagura_no" + PassAttr).str();
  if (hasAnnotation(F, DisableAttr))
    return false;
  if (hasAnnotation(F, EnableAttr))
    return true;
  return GlobalFlag;
}

// ---- PRNG ----

// Note: the -kagura-seed option is declared in Plugin.cpp to avoid
// duplicate registration. Access via getModulePRNG() which reads it.

PRNG::PRNG(uint64_t Seed) {
  if (Seed == 0) {
    // Mix time-based and std::random_device entropy
    std::random_device RD;
    auto T = std::chrono::steady_clock::now().time_since_epoch().count();
    State = static_cast<uint64_t>(RD()) ^ static_cast<uint64_t>(T);
  } else {
    State = Seed;
  }
}

uint64_t PRNG::next() {
  uint64_t Z = (State += 0x9e3779b97f4a7c15ULL);
  Z = (Z ^ (Z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  Z = (Z ^ (Z >> 27)) * 0x94d049bb133111ebULL;
  return Z ^ (Z >> 31);
}

uint32_t PRNG::next32() { return static_cast<uint32_t>(next()); }

uint64_t PRNG::nextRange(uint64_t Lo, uint64_t Hi) {
  assert(Hi > Lo);
  return Lo + (next() % (Hi - Lo));
}

// 2026-10-08 (K1): -kagura-seed is a cl::opt registered inside the pass
// plugin, but `opt` runs cl::ParseCommandLineOptions() BEFORE loading
// -load-pass-plugin, so the option is unknown at parse time and the flag can
// never reach us (verified: -kagura-seed / --kagura-seed / spaced form all
// produce "Unknown command line argument").  Without a seed every build is
// reseeded from random_device ^ steady_clock -> obfuscation output differs on
// every build and a bad build can neither be reproduced nor bisected.
// Fall back to the KAGURA_SEED environment variable, which survives the
// plugin load order.
static uint64_t readSeedFromEnv() {
  const char *S = std::getenv("KAGURA_SEED");
  if (!S || !*S)
    return 0;
  return static_cast<uint64_t>(std::strtoull(S, nullptr, 0));
}

PRNG &getModulePRNG() {
  // 2026-09-25 (audit P2-3): C++11 magic static — the initialiser runs
  // exactly once and is thread-safe even under parallel LTO backends.
  // The previous hand-rolled `static bool Seeded` flag had a data race.
  static PRNG GlobalPRNG = [] {
    uint64_t BaseSeed = opt::Seed ? opt::Seed : readSeedFromEnv();
    // 4.2.7: Mix BuildID into seed for per-build key rotation.
    // Using FNV-1a over the BuildID string ensures unique keys even when
    // the user specifies a fixed -kagura-seed for reproducible output.
    // Same plugin-load-order caveat as KAGURA_SEED above.
    const char *EnvID = std::getenv("KAGURA_BUILD_ID");
    std::string BuildIDStr = opt::BuildID.empty()
        ? (EnvID ? std::string(EnvID) : std::string())
        : opt::BuildID.getValue();
    if (!BuildIDStr.empty()) {
      uint64_t Hash = 0xcbf29ce484222325ULL; // FNV offset basis
      for (char C : BuildIDStr) {
        Hash ^= static_cast<uint8_t>(C);
        Hash *= 0x100000001b3ULL; // FNV prime
      }
      BaseSeed ^= Hash;
    }
    // 2026-10-08 (K1): make the effective seed traceable — with a fixed seed
    // two builds must produce byte-identical IR, which is what makes a bad
    // build reproducible and bisectable.
    llvm::errs() << "[kagura] effective seed = " << BaseSeed << "\n";
    return PRNG(BaseSeed);
  }();
  return GlobalPRNG;
}

// ---- Exception-handling safety ----

bool hasExceptionHandling(const Function &F) {
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (isa<InvokeInst>(I) || isa<LandingPadInst>(I) ||
          isa<ResumeInst>(I) || isa<CleanupPadInst>(I) ||
          isa<CatchPadInst>(I) || isa<CatchReturnInst>(I) ||
          isa<CleanupReturnInst>(I))
        return true;
  return false;
}

bool isEHBlock(const BasicBlock &BB) {
  const Instruction *First = &BB.front();
  if (isa<LandingPadInst>(First) || isa<CleanupPadInst>(First) ||
      isa<CatchPadInst>(First))
    return true;
  for (const Instruction &I : BB)
    if (isa<LandingPadInst>(I) || isa<CleanupPadInst>(I) ||
        isa<CatchPadInst>(I) || isa<ResumeInst>(I))
      return true;
  return false;
}

// ---- Target triple helpers ----

// Overload set: converts whichever type getTargetTriple() returns to std::string.
// LLVM 17-19 returns const std::string &; LLVM 20+ returns const Triple &.
static std::string tripleToString(const std::string &S) { return S; }
static std::string tripleToString(const llvm::Triple &T) { return T.str(); }

std::string getModuleTriple(const Module &M) {
  return tripleToString(M.getTargetTriple());
}

TargetArch getTargetArch(const Module &M) {
  std::string TripleStr = getModuleTriple(M);
  StringRef Triple(TripleStr);
  // arm64e must be checked before generic aarch64 since it is a substring match.
  if (Triple.contains("arm64e"))
    return TargetArch::ARM64e;
  if (Triple.starts_with("aarch64") || Triple.starts_with("arm64"))
    return TargetArch::ARM64;
  if (Triple.starts_with("armv7") || Triple.starts_with("thumbv7"))
    return TargetArch::ARMv7;
  if (Triple.starts_with("x86_64") || Triple.starts_with("amd64"))
    return TargetArch::X86_64;
  if (Triple.starts_with("wasm64"))
    return TargetArch::Wasm64;
  if (Triple.starts_with("wasm32"))
    return TargetArch::Wasm32;
  return TargetArch::Other;
}

bool isAArch64Target(const Module &M) {
  TargetArch A = getTargetArch(M);
  return A == TargetArch::ARM64 || A == TargetArch::ARM64e;
}

bool isArm64eTarget(const Module &M) {
  return getTargetArch(M) == TargetArch::ARM64e;
}

bool isARMv7Target(const Module &M) {
  return getTargetArch(M) == TargetArch::ARMv7;
}

bool isX86_64Target(const Module &M) {
  return getTargetArch(M) == TargetArch::X86_64;
}

bool isWasmTarget(const Module &M) {
  TargetArch A = getTargetArch(M);
  return A == TargetArch::Wasm32 || A == TargetArch::Wasm64;
}

// ---- IR helpers ----

void demotePhis(Function &F) {
  // Full reg2mem: demote both PHI nodes and any non-PHI instruction whose
  // uses cross a basic-block boundary.  FLA rewires the entire CFG into a
  // switch dispatch, so the old dominator tree is invalidated — every
  // cross-block SSA use would violate dominance in the new CFG.
  //
  // Process PHIs first (they must be demoted before the instructions that
  // use their results, otherwise DemoteRegToStack trips on a PHI user).
  std::vector<PHINode *> Phis;
  for (auto &BB : F)
    for (auto &I : BB)
      if (auto *Phi = dyn_cast<PHINode>(&I))
        Phis.push_back(Phi);
  for (auto *Phi : Phis)
    DemotePHIToStack(Phi);

  // Now demote non-PHI instructions with cross-block uses.
  // Skip: terminators, instructions with no uses, allocas (already memory),
  // and any instruction in the entry block whose only users are in the same
  // block (avoids demoting entry-block stores/loads that reference args).
  std::vector<Instruction *> ToSpill;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (isa<PHINode>(I) || I.isTerminator() || I.use_empty())
        continue;
      // Never demote alloca instructions — they are already memory references.
      if (isa<AllocaInst>(I))
        continue;
      for (auto *U : I.users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (UI && UI->getParent() != &BB) {
          ToSpill.push_back(&I);
          break;
        }
      }
    }
  }
  for (auto *I : ToSpill)
    DemoteRegToStack(*I, /*VolatileLoads=*/false);
}

std::vector<BasicBlock *> getBlocks(Function &F) {
  std::vector<BasicBlock *> Blocks;
  for (auto &BB : F)
    Blocks.push_back(&BB);
  return Blocks;
}

Function *getOrDeclare(Module &M, StringRef Name, FunctionType *FTy) {
  if (auto *F = M.getFunction(Name))
    return F;
  return Function::Create(FTy, Function::ExternalLinkage, Name, M);
}

// ---- String global collection ----

std::vector<GlobalVariable *> collectStringGlobals(Module &M,
                                                    bool StrictLinkage) {
  std::vector<GlobalVariable *> Result;
  for (auto &GV : M.globals()) {
    if (!GV.isConstant() || !GV.hasInitializer())
      continue;
    if (StrictLinkage &&
        GV.getLinkage() != GlobalValue::PrivateLinkage &&
        GV.getLinkage() != GlobalValue::InternalLinkage)
      continue;
    auto *CDA = dyn_cast<ConstantDataArray>(GV.getInitializer());
    if (!CDA || !CDA->isString())
      continue;
    StringRef S = CDA->getAsString();
    if (S.size() < 4)
      continue;
    if (S.contains('%') || S.trim().empty())
      continue;
    bool UsedInFunction = false;
    for (auto *U : GV.users()) {
      if (isa<Instruction>(U) ||
          (isa<ConstantExpr>(U) && !cast<ConstantExpr>(U)->user_empty())) {
        UsedInFunction = true;
        break;
      }
    }
    if (UsedInFunction)
      Result.push_back(&GV);
  }
  return Result;
}

// ---- Constant builders ----

Constant *buildByteArrayConstant(LLVMContext &Ctx, ArrayRef<uint8_t> Data) {
  auto *Int8Ty = Type::getInt8Ty(Ctx);
  auto *ArrTy  = ArrayType::get(Int8Ty, Data.size());
  std::vector<Constant *> Bytes;
  Bytes.reserve(Data.size());
  for (uint8_t B : Data)
    Bytes.push_back(ConstantInt::get(Int8Ty, B));
  return ConstantArray::get(ArrTy, Bytes);
}

GlobalVariable *createPrivateByteGlobal(Module &M, ArrayRef<uint8_t> Data,
                                         StringRef Name, bool IsConstant) {
  LLVMContext &Ctx = M.getContext();
  auto *Init = buildByteArrayConstant(Ctx, Data);
  return new GlobalVariable(M, Init->getType(), IsConstant,
                            GlobalValue::PrivateLinkage, Init, Name);
}

void fillRandomBytes(uint8_t *Out, size_t Len) {
  PRNG &RNG = getModulePRNG();
  for (size_t I = 0; I < Len; I += 8) {
    uint64_t V = RNG.next();
    size_t N = std::min(static_cast<size_t>(8), Len - I);
    for (size_t J = 0; J < N; ++J)
      Out[I + J] = static_cast<uint8_t>((V >> (J * 8)) & 0xFF);
  }
}

} // namespace kagura
