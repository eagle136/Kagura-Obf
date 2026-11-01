//===-- ConstantObfuscation.cpp - Integer/float constant MBA obfuscation --===//
//
// Replaces integer and floating-point constant operands with equivalent
// computations that are harder to reason about statically.
//
// Integer MBA identities (for 32/64-bit integers, value V):
//   1. V  =>  (V ^ R) ^ R                     (double XOR with random R)
//   2. V  =>  (V + R) - R                     (add/sub with random R)
//   3. V  =>  ((V | ~V) & V)                  (identity via tautology)
//   4. V  =>  (V * R) * modular_inverse(R)    (multiplicative, R must be odd)
//   5. V  =>  ~(~V + K) + K                   (double complement)
//   6. V  =>  (V ^ R1) + R2 - R1 ^ R2         (chained XOR/ADD)
//   7. V  =>  (V | K) & (V | ~K)              (mask identity)
//   8. V  =>  (V * R) >> S 补偿               (shift-mul combine)
//
// 4.2.9: Float/double constant obfuscation:
//   Float F  =>  bitcast( bitcast(F) ^ R ^ R )  (XOR through integer domain)
//   Double D =>  bitcast( bitcast(D) ^ R ^ R )  (same idea for 64-bit)
//
// Each constant is replaced with a randomly chosen identity.
// Only replaces constants in user functions, not in kagura's own helpers.
//
//===----------------------------------------------------------------------===//

#include "kagura/Passes.h"
#include "kagura/Utils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <vector>

using namespace llvm;

namespace kagura {

// Compute modular multiplicative inverse of odd integer x mod 2^bits
// using extended Euclidean algorithm (Newton's method for 2^n).
static uint64_t modInverse(uint64_t x, unsigned bits) {
  assert(x & 1 && "x must be odd for modular inverse to exist");
  uint64_t inv = 1;
  for (unsigned i = 0; i < bits - 1; ++i)
    inv = inv * (2 - x * inv);
  if (bits < 64)
    inv &= (1ULL << bits) - 1;
  return inv;
}

// Replace a ConstantInt CI (used as operand OpIdx in instruction I) with an
// MBA expression that evaluates to the same value.
static bool obfuscateConstant(Instruction *I, unsigned OpIdx,
                               ConstantInt *CI, PRNG &RNG) {
  // Only handle i8..i64
  unsigned Bits = CI->getBitWidth();
  if (Bits < 8 || Bits > 64)
    return false;

  // Skip zero and all-ones (they appear in many structural patterns)
  uint64_t Val = CI->getZExtValue();
  if (Val == 0 || Val == ((Bits == 64) ? ~0ULL : (1ULL << Bits) - 1))
    return false;

  IRBuilder<> B(I);
  Type *Ty = CI->getType();

  // Pick a random identity transformation (8 variants now)
  uint32_t Choice = RNG.next32() % 8;
  Value *Result = nullptr;

  switch (Choice) {
  case 0: {
    // (V ^ R) ^ R
    uint64_t R = RNG.next() & ((Bits == 64) ? ~0ULL : (1ULL << Bits) - 1);
    auto *RC  = ConstantInt::get(Ty, R);
    auto *Xor1 = B.CreateXor(CI, RC, "co.xr1");
    Result     = B.CreateXor(Xor1, RC, "co.xr2");
    break;
  }
  case 1: {
    // (V + R) - R
    uint64_t R = RNG.next() & ((Bits == 64) ? ~0ULL : (1ULL << Bits) - 1);
    auto *RC   = ConstantInt::get(Ty, R);
    auto *Add  = B.CreateAdd(CI, RC, "co.ar1");
    Result     = B.CreateSub(Add, RC, "co.ar2");
    break;
  }
  case 2: {
    // ((V | ~V) & V)  — always == V since (V | ~V) == all-ones
    auto *NotV = B.CreateNot(CI, "co.notv");
    auto *Or   = B.CreateOr(CI, NotV, "co.or");
    Result     = B.CreateAnd(Or, CI, "co.and");
    break;
  }
  case 3: {
    // (V * R) * R_inv   where R is a random odd number
    uint64_t Mask  = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
    uint64_t R     = (RNG.next() | 1) & Mask; // force odd
    uint64_t RInv  = modInverse(R, Bits) & Mask;
    auto *RC       = ConstantInt::get(Ty, R);
    auto *RInvC    = ConstantInt::get(Ty, RInv);
    auto *Mul1     = B.CreateMul(CI, RC, "co.mr1");
    Result         = B.CreateMul(Mul1, RInvC, "co.mr2");
    break;
  }
  case 4: {
    // ~(~V + K) + K   (double complement with add)
    uint64_t K = RNG.next() & ((Bits == 64) ? ~0ULL : (1ULL << Bits) - 1);
    auto *KC   = ConstantInt::get(Ty, K);
    auto *NotV = B.CreateNot(CI, "co.notv");
    auto *Add  = B.CreateAdd(NotV, KC, "co.add");
    auto *NotA = B.CreateNot(Add, "co.nota");
    Result     = B.CreateAdd(NotA, KC, "co.dc");
    break;
  }
  case 5: {
    // 2026-09-23 FIX: the old chained form (V^R1)+R2-(R1^R2) was NOT an
    // identity (counterexample V=2, R1=1, R2=1 -> 4 != 2) and silently
    // corrupted constants.  Replaced with an exact chained identity:
    //   ((V + R1) ^ R2) ^ R2 - R1  ==  V   for all V, R1, R2 (mod 2^n)
    uint64_t Mask = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
    uint64_t R1   = RNG.next() & Mask;
    uint64_t R2   = RNG.next() & Mask;
    auto *R1C     = ConstantInt::get(Ty, R1);
    auto *R2C     = ConstantInt::get(Ty, R2);
    auto *Add1    = B.CreateAdd(CI, R1C, "co.ca1");
    auto *Xor1    = B.CreateXor(Add1, R2C, "co.cx1");
    auto *Xor2    = B.CreateXor(Xor1, R2C, "co.cx2");
    Result        = B.CreateSub(Xor2, R1C, "co.cs1");
    break;
  }
  case 6: {
    // (V | K) & (V | ~K)  == V
    uint64_t K = RNG.next() & ((Bits == 64) ? ~0ULL : (1ULL << Bits) - 1);
    auto *KC   = ConstantInt::get(Ty, K);
    auto *NotK = ConstantInt::get(Ty, ~K & ((Bits == 64) ? ~0ULL : (1ULL << Bits) - 1));
    auto *Or1  = B.CreateOr(CI, KC, "co.ok1");
    auto *Or2  = B.CreateOr(CI, NotK, "co.ok2");
    Result     = B.CreateAnd(Or1, Or2, "co.mk");
    break;
  }
  case 7: {
    // (V & K) | (V & ~K)  == V  (mask/unmask distribution identity)
    // This always holds: (V & K) | (V & ~K) = V  for any K
    uint64_t Mask = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
    uint64_t K = RNG.next() & Mask;
    auto *KC   = ConstantInt::get(Ty, K);
    auto *NotK = ConstantInt::get(Ty, ~K & Mask);
    auto *And1 = B.CreateAnd(CI, KC, "co.aki");
    auto *And2 = B.CreateAnd(CI, NotK, "co.ak2");
    Result     = B.CreateOr(And1, And2, "co.mki");
    break;
  }
  }

  if (!Result)
    return false;

  I->setOperand(OpIdx, Result);
  return true;
}

// 4.2.9: Obfuscate a float or double constant by XOR-ing the bit representation
// with a random integer key in the integer domain, then bitcasting back.
static bool obfuscateFPConstant(Instruction *I, unsigned OpIdx,
                                 ConstantFP *CFP, PRNG &RNG) {
  LLVMContext &Ctx = I->getContext();
  Type *FTy = CFP->getType();
  bool IsDouble = FTy->isDoubleTy();
  bool IsFloat  = FTy->isFloatTy();
  if (!IsFloat && !IsDouble)
    return false;

  unsigned Bits = IsDouble ? 64 : 32;
  Type *ITy = IsDouble ? Type::getInt64Ty(Ctx) : Type::getInt32Ty(Ctx);

  uint64_t FBits = IsDouble
      ? CFP->getValueAPF().bitcastToAPInt().getZExtValue()
      : (uint64_t)CFP->getValueAPF().bitcastToAPInt().getZExtValue();

  uint64_t R = RNG.next() & ((Bits == 64) ? ~0ULL : 0xFFFFFFFFULL);

  IRBuilder<> B(I);
  // Build: bitcast_fp( bitcast_int(FP_constant) ^ R ^ R )
  // The two XORs cancel — but the pattern is non-trivial for static analysis.
  auto *FBitsConst = ConstantInt::get(ITy, FBits);
  auto *RConst     = ConstantInt::get(ITy, R);
  auto *Xor1 = B.CreateXor(FBitsConst, RConst, "co.fxr1");
  auto *Xor2 = B.CreateXor(Xor1, RConst, "co.fxr2");
  auto *FPResult = B.CreateBitCast(Xor2, FTy, "co.fp");

  I->setOperand(OpIdx, FPResult);
  return true;
}

static bool obfuscateFunction(Function &F, PRNG &RNG) {
  bool Changed = false;

  // FIX: Use a worklist to avoid infinite loop from newly inserted instructions
  // in the same BB being iterated over by the range-for loop.
  struct ConstantRef {
    Instruction *I;
    unsigned OpIdx;
    ConstantInt *CI;
    ConstantFP *CFP;
    bool IsInt;
  };
  std::vector<ConstantRef> Worklist;

  for (auto &BB : F) {
    if (BB.getName().starts_with("kagura."))
      continue;
    for (auto &I : BB) {
      if (isa<PHINode>(&I) || isa<AllocaInst>(&I))
        continue;
      if (isa<GetElementPtrInst>(&I))
        continue;
      if (isa<CallInst>(&I) || isa<InvokeInst>(&I))
        continue;
      for (unsigned OpIdx = 0; OpIdx < I.getNumOperands(); ++OpIdx) {
        Value *Op = I.getOperand(OpIdx);
        // 50% chance per constant (was 30%) for stronger obfuscation
        if (RNG.nextRange(0, 100) >= 50)
          continue;
        if (auto *CI = dyn_cast<ConstantInt>(Op)) {
          Worklist.push_back({&I, OpIdx, CI, nullptr, true});
        } else if (auto *CFP = dyn_cast<ConstantFP>(Op)) {
          Worklist.push_back({&I, OpIdx, nullptr, CFP, false});
        }
      }
    }
  }

  // Process collected worklist — this is safe, no new items added during iteration
  for (auto &Ref : Worklist) {
    if (Ref.IsInt) {
      Changed |= obfuscateConstant(Ref.I, Ref.OpIdx, Ref.CI, RNG);
    } else {
      Changed |= obfuscateFPConstant(Ref.I, Ref.OpIdx, Ref.CFP, RNG);
    }
  }

  return Changed;
}

PreservedAnalyses ConstantObfuscationPass::run(Function &F,
                                                FunctionAnalysisManager &) {
  if (!shouldObfuscate(F, "co", true))
    return PreservedAnalyses::all();
  auto &RNG    = getModulePRNG();
  bool Changed = obfuscateFunction(F, RNG);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace kagura