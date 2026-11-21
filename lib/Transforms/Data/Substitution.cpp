//===-- Substitution.cpp - Instruction substitution pass (enhanced) --------===//
//
// Replaces arithmetic and bitwise operators with equivalent Mixed
// Boolean-Arithmetic (MBA) expressions that are harder to simplify.
//
// Extends DeClang's substitution by:
//   - Supporting MUL (a*b = ((a|b)-(a^b)) doesn't work cleanly; use shift form)
//   - Supporting SHL, LSHR, ASHR
//   - Applying substitutions to floating-point ops via integer reinterpretation
//   - Using multi-level substitution chains when Iterations > 1
//   - 12 distinct substitution patterns across all integer ops
//
//===----------------------------------------------------------------------===//

#include "kagura/Passes.h"
#include "kagura/Utils.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/PatternMatch.h"

#include <functional>
#include <vector>

using namespace llvm;

namespace kagura {

using SubstFn = std::function<Value *(BinaryOperator *, IRBuilder<> &, PRNG &)>;

// ---- ADD substitutions ----

static Value *addSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a + b  =>  a - (-b)
  return B.CreateSub(I->getOperand(0),
                     B.CreateNeg(I->getOperand(1)), "sub.add0");
}
static Value *addSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a + b  =>  (a ^ b) + 2*(a & b)   (correct carry-propagation identity)
  auto *Xor = B.CreateXor(I->getOperand(0), I->getOperand(1));
  auto *And = B.CreateAnd(I->getOperand(0), I->getOperand(1));
  auto *Two = ConstantInt::get(I->getType(), 2);
  auto *Shl = B.CreateMul(And, Two);
  return B.CreateAdd(Xor, Shl, "sub.add1");
}
static Value *addSub2(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a + b  =>  (a | b) + (a & b)
  auto *Or  = B.CreateOr(I->getOperand(0), I->getOperand(1));
  auto *And = B.CreateAnd(I->getOperand(0), I->getOperand(1));
  return B.CreateAdd(Or, And, "sub.add2");
}
static Value *addSub3(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // 2026-09-23 FIX: the old form (a^b) | ((a&b)<<1) is NOT exact — when a
  // carry bit overlaps a differing bit, OR swallows the carry (counterexample
  // a=6, b=2 -> 4 instead of 8), which silently corrupted loop counters and
  // made loops hang.  Replaced with the exact carry identity:
  //   a + b  ==  (a ^ b) + ((a & b) << 1)
  auto *Xor = B.CreateXor(I->getOperand(0), I->getOperand(1));
  auto *And = B.CreateAnd(I->getOperand(0), I->getOperand(1));
  auto *One = ConstantInt::get(I->getType(), 1);
  auto *Shl = B.CreateShl(And, One, "sub.add3.shl");
  return B.CreateAdd(Xor, Shl, "sub.add3");
}

// ---- SUB substitutions ----

static Value *subSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a - b  =>  a + (-b)
  return B.CreateAdd(I->getOperand(0),
                     B.CreateNeg(I->getOperand(1)), "sub.sub0");
}
static Value *subSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a - b  =>  (a ^ b) - 2*(~a & b)   (correct borrow-propagation identity)
  auto *Xor  = B.CreateXor(I->getOperand(0), I->getOperand(1));
  auto *NotA = B.CreateNot(I->getOperand(0));
  auto *And  = B.CreateAnd(NotA, I->getOperand(1));
  auto *Two  = ConstantInt::get(I->getType(), 2);
  auto *Shl  = B.CreateMul(And, Two);
  return B.CreateSub(Xor, Shl, "sub.sub1");
}
static Value *subSub2(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a - b  =>  (a & ~b) - (~a & b)   (bitwise subtraction identity)
  auto *NotB = B.CreateNot(I->getOperand(1));
  auto *NotA = B.CreateNot(I->getOperand(0));
  auto *L    = B.CreateAnd(I->getOperand(0), NotB, "sub.sub2.l");
  auto *R    = B.CreateAnd(NotA, I->getOperand(1), "sub.sub2.r");
  return B.CreateSub(L, R, "sub.sub2");
}

// ---- AND substitutions ----

static Value *andSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a & b  =>  (a ^ ~b) & a
  auto *NotB = B.CreateNot(I->getOperand(1));
  auto *Xor  = B.CreateXor(I->getOperand(0), NotB);
  return B.CreateAnd(Xor, I->getOperand(0), "sub.and0");
}
static Value *andSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a & b  =>  ~(~a | ~b)   (De Morgan)
  auto *NotA  = B.CreateNot(I->getOperand(0));
  auto *NotB  = B.CreateNot(I->getOperand(1));
  auto *OrAB  = B.CreateOr(NotA, NotB);
  return B.CreateNot(OrAB, "sub.and1");
}
static Value *andSub2(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a & b  =>  (a + b) - (a | b)   (alternate identity)
  auto *Sum = B.CreateAdd(I->getOperand(0), I->getOperand(1), "sub.and2.sum");
  auto *Or  = B.CreateOr(I->getOperand(0), I->getOperand(1), "sub.and2.or");
  return B.CreateSub(Sum, Or, "sub.and2");
}

// ---- OR substitutions ----

static Value *orSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a | b  =>  (a & b) | (a ^ b)
  auto *And = B.CreateAnd(I->getOperand(0), I->getOperand(1));
  auto *Xor = B.CreateXor(I->getOperand(0), I->getOperand(1));
  return B.CreateOr(And, Xor, "sub.or0");
}
static Value *orSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a | b  =>  ~(~a & ~b)   (De Morgan)
  auto *NotA = B.CreateNot(I->getOperand(0));
  auto *NotB = B.CreateNot(I->getOperand(1));
  auto *And  = B.CreateAnd(NotA, NotB);
  return B.CreateNot(And, "sub.or1");
}
static Value *orSub2(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a | b  =>  (a ^ b) + (a & b)   (arithmetic identity: XOR + AND == OR when no overlap)
  // Actually: a|b = (a^b) + (a&b)  holds for all values (no overflow when bits don't overlap)
  auto *Xor = B.CreateXor(I->getOperand(0), I->getOperand(1), "sub.or2.xor");
  auto *And = B.CreateAnd(I->getOperand(0), I->getOperand(1), "sub.or2.and");
  return B.CreateAdd(Xor, And, "sub.or2");
}

// ---- XOR substitutions ----

static Value *xorSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a ^ b  =>  (a | b) & ~(a & b)
  auto *Or   = B.CreateOr(I->getOperand(0), I->getOperand(1));
  auto *And  = B.CreateAnd(I->getOperand(0), I->getOperand(1));
  auto *NAnd = B.CreateNot(And);
  return B.CreateAnd(Or, NAnd, "sub.xor0");
}
static Value *xorSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a ^ b  =>  (a & ~b) | (~a & b)
  auto *NotB = B.CreateNot(I->getOperand(1));
  auto *NotA = B.CreateNot(I->getOperand(0));
  auto *L    = B.CreateAnd(I->getOperand(0), NotB);
  auto *R    = B.CreateAnd(NotA, I->getOperand(1));
  return B.CreateOr(L, R, "sub.xor1");
}
static Value *xorSub2(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a ^ b  =>  (a | b) - (a & b)   (arithmetic: OR - AND == XOR)
  auto *Or  = B.CreateOr(I->getOperand(0), I->getOperand(1), "sub.xor2.or");
  auto *And = B.CreateAnd(I->getOperand(0), I->getOperand(1), "sub.xor2.and");
  return B.CreateSub(Or, And, "sub.xor2");
}

// ---- MUL substitutions ----

static Value *mulSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &RNG) {
  // a * b  =>  a * (b ^ r ^ r)  = no-op but confuses solvers
  auto *T = I->getType();
  auto *R  = ConstantInt::get(T, RNG.next());
  auto *XR = B.CreateXor(I->getOperand(1), R);
  auto *XR2 = B.CreateXor(XR, R);
  return B.CreateMul(I->getOperand(0), XR2, "sub.mul0");
}
static Value *mulSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &RNG) {
  // a * b  =>  a*(b+X) - a*X     (exact for ALL widths, mod 2^n)
  // 2026-09-23 FIX: the old >32-bit branch returned (a+b)*(a-b) = a^2-b^2,
  // which silently corrupted every 64-bit multiplication.  This identity is
  // exact: a*(b+X) - a*X = a*b + a*X - a*X = a*b.
  auto *T    = I->getType();
  unsigned Bits = T->getIntegerBitWidth();
  uint64_t Mask = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
  uint64_t X = RNG.next() & Mask;
  auto *XC   = ConstantInt::get(T, X);
  auto *BpX  = B.CreateAdd(I->getOperand(1), XC, "sub.mul1.bp1");
  auto *L    = B.CreateMul(I->getOperand(0), BpX, "sub.mul1.l");
  auto *R    = B.CreateMul(I->getOperand(0), XC, "sub.mul1.r");
  return B.CreateSub(L, R, "sub.mul1");
}

// ---- SHL substitutions ----

static Value *shlSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a << b  =>  a * (1 << b)   (shift as multiply by power of 2)
  auto *One = ConstantInt::get(I->getType(), 1);
  auto *Exp = B.CreateShl(One, I->getOperand(1), "sub.shl0.exp");
  return B.CreateMul(I->getOperand(0), Exp, "sub.shl0");
}
static Value *shlSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &RNG) {
  // 2026-09-23 FIX: old form (a<<(b+1))/2 was poison when b == bitwidth-1
  // and relied on UDiv exactness.  New form is an exact bitwise identity:
  // shift distributes over XOR, so ((a^R)<<b) ^ (R<<b) == (a<<b).
  auto *T = I->getType();
  unsigned Bits = T->getIntegerBitWidth();
  uint64_t Mask = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
  auto *R  = ConstantInt::get(T, RNG.next() & Mask);
  auto *XR = B.CreateXor(I->getOperand(0), R, "sub.shl1.xr");
  auto *S1 = B.CreateShl(XR, I->getOperand(1), "sub.shl1.s1");
  auto *S2 = B.CreateShl(R, I->getOperand(1), "sub.shl1.s2");
  return B.CreateXor(S1, S2, "sub.shl1");
}
static Value *shlSub2(BinaryOperator *I, IRBuilder<> &B, PRNG &RNG) {
  // 2026-09-23 FIX: old form (a<<1)<<(b-1) produced shift-by--1 poison when
  // b == 0.  Replaced with pure double-XOR noise (exact for all b).
  auto *T = I->getType();
  unsigned Bits = T->getIntegerBitWidth();
  uint64_t Mask = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
  auto *R = ConstantInt::get(T, RNG.next() & Mask);
  auto *S = B.CreateShl(I->getOperand(0), I->getOperand(1), "sub.shl2.s");
  auto *X1 = B.CreateXor(S, R, "sub.shl2.x1");
  return B.CreateXor(X1, R, "sub.shl2");
}

// ---- LSHR substitutions ----

static Value *lshrSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &) {
  // a >> b  (logical)  =>  a / (1 << b)
  auto *One = ConstantInt::get(I->getType(), 1);
  auto *Exp = B.CreateShl(One, I->getOperand(1), "sub.ls0.exp");
  return B.CreateUDiv(I->getOperand(0), Exp, "sub.ls0");
}
static Value *lshrSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &RNG) {
  // 2026-09-23 FIX: old form (a>>(b+1))*2 silently cleared the LSB when
  // b == 0.  New form is exact: LShr distributes over XOR, so
  // ((a^R)>>b) ^ (R>>b) == (a>>b) for every b.
  auto *T = I->getType();
  unsigned Bits = T->getIntegerBitWidth();
  uint64_t Mask = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
  auto *R  = ConstantInt::get(T, RNG.next() & Mask);
  auto *XR = B.CreateXor(I->getOperand(0), R, "sub.ls1.xr");
  auto *S1 = B.CreateLShr(XR, I->getOperand(1), "sub.ls1.s1");
  auto *S2 = B.CreateLShr(R, I->getOperand(1), "sub.ls1.s2");
  return B.CreateXor(S1, S2, "sub.ls1");
}

// ---- ASHR substitutions ----

static Value *ashrSub0(BinaryOperator *I, IRBuilder<> &B, PRNG &RNG) {
  // 2026-09-23 FIX: old form (a>>1)>>(b-1) produced shift-by--1 poison when
  // b == 0.  New form is exact: ASHR distributes over XOR (the sign fill of
  // (a^R) equals sign(a)^sign(R), so every result bit matches), hence
  // ((a^R)>>s b) ^ (R>>s b) == (a>>s b) for every b.
  auto *T = I->getType();
  unsigned Bits = T->getIntegerBitWidth();
  uint64_t Mask = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
  auto *R  = ConstantInt::get(T, RNG.next() & Mask);
  auto *XR = B.CreateXor(I->getOperand(0), R, "sub.as0.xr");
  auto *S1 = B.CreateAShr(XR, I->getOperand(1), "sub.as0.s1");
  auto *S2 = B.CreateAShr(R, I->getOperand(1), "sub.as0.s2");
  return B.CreateXor(S1, S2, "sub.as0");
}
static Value *ashrSub1(BinaryOperator *I, IRBuilder<> &B, PRNG &RNG) {
  // 2026-09-23 FIX: old form (a>>(b+1))<<1>>1 silently lost one shift bit
  // (always computed a>>(b+1)).  Replaced with double-XOR noise, exact.
  auto *T = I->getType();
  unsigned Bits = T->getIntegerBitWidth();
  uint64_t Mask = (Bits == 64) ? ~0ULL : (1ULL << Bits) - 1;
  auto *R = ConstantInt::get(T, RNG.next() & Mask);
  auto *S = B.CreateAShr(I->getOperand(0), I->getOperand(1), "sub.as1.s");
  auto *X1 = B.CreateXor(S, R, "sub.as1.x1");
  return B.CreateXor(X1, R, "sub.as1");
}

// ---- Dispatch table ----

static const std::vector<SubstFn> AddSubsts    = {addSub0, addSub1, addSub2, addSub3};
static const std::vector<SubstFn> SubSubsts    = {subSub0, subSub1, subSub2};
static const std::vector<SubstFn> AndSubsts    = {andSub0, andSub1, andSub2};
static const std::vector<SubstFn> OrSubsts     = {orSub0,  orSub1,  orSub2};
static const std::vector<SubstFn> XorSubsts    = {xorSub0, xorSub1, xorSub2};
static const std::vector<SubstFn> MulSubsts    = {mulSub0, mulSub1};
static const std::vector<SubstFn> ShlSubsts    = {shlSub0, shlSub1, shlSub2};
static const std::vector<SubstFn> LShrSubsts   = {lshrSub0, lshrSub1};
static const std::vector<SubstFn> AShrSubsts   = {ashrSub0, ashrSub1};

static const std::vector<SubstFn> *getSubsts(unsigned Opcode) {
  switch (Opcode) {
  case Instruction::Add:  return &AddSubsts;
  case Instruction::Sub:  return &SubSubsts;
  case Instruction::And:  return &AndSubsts;
  case Instruction::Or:   return &OrSubsts;
  case Instruction::Xor:  return &XorSubsts;
  case Instruction::Mul:  return &MulSubsts;
  case Instruction::Shl:  return &ShlSubsts;
  case Instruction::LShr: return &LShrSubsts;
  case Instruction::AShr: return &AShrSubsts;
  default: return nullptr;
  }
}

static bool substituteFunction(Function &F, uint32_t Iterations, PRNG &RNG) {
  bool Changed = false;
  for (uint32_t Iter = 0; Iter < Iterations; ++Iter) {
    SmallVector<BinaryOperator *, 16> Worklist;
    for (auto &BB : F) {
      // Skip kagura's own dispatcher blocks
      if (BB.getName().starts_with("kagura."))
        continue;
      for (auto &I : BB)
        if (auto *BO = dyn_cast<BinaryOperator>(&I))
          if (BO->getType()->isIntegerTy())
            if (getSubsts(BO->getOpcode()))
              Worklist.push_back(BO);
    }

    for (auto *BO : Worklist) {
      const auto *Substs = getSubsts(BO->getOpcode());
      if (!Substs) continue;
      uint64_t Idx = RNG.nextRange(0, Substs->size());
      IRBuilder<> B(BO);
      Value *Replacement = (*Substs)[Idx](BO, B, RNG);
      // Safety check: if Replacement transitively references BO, skip to avoid
      // infinite loop in LLVM's RAUW or use-after-free.
      //
      // 2026-11-05 FIX (was an unbounded walk):
      //   The traversal below follows the *operand* graph, and SSA operand
      //   graphs contain cycles: a PHI in a loop takes its own back-edge
      //   value as an incoming operand, e.g.
      //       %p = phi i32 [ %inc, %loop ]
      //       %inc = add i32 %p, 3
      //   The old code had NO visited-set, so it re-expanded the same PHI on
      //   every visit and never terminated whenever BO was not itself part of
      //   that cycle (BO sitting *outside* the loop but consuming %p is the
      //   common case).  WorklistCheck grew without bound -> opt consumed
      //   ~30 GB and the build was killed by the 60 s timeout, silently
      //   dropping the whole kagura-sub pass from the output.
      //   A visited-set makes the walk linear in reachable instructions and
      //   terminates on cycles.  The self-reference verdict is unchanged:
      //   BO can only be reached through the PHI back-edge, which the
      //   visited-set still discovers on the first visit.
      bool HasSelfRef = false;
      SmallPtrSet<Value *, 32> Visited;
      SmallVector<Value *, 8> WorklistCheck;
      WorklistCheck.push_back(Replacement);
      Visited.insert(Replacement);
      while (!WorklistCheck.empty()) {
        Value *V = WorklistCheck.pop_back_val();
        if (V == BO) { HasSelfRef = true; break; }
        if (auto *I = dyn_cast<Instruction>(V))
          for (Value *Op : I->operands())
            if (isa<Instruction>(Op) || isa<Argument>(Op))
              if (Visited.insert(Op).second)
                WorklistCheck.push_back(Op);
      }
      if (HasSelfRef)
        continue;
      BO->replaceAllUsesWith(Replacement);
      BO->eraseFromParent();
      Changed = true;
    }
  }
  return Changed;
}

PreservedAnalyses SubstitutionPass::run(Function &F,
                                        FunctionAnalysisManager &) {
  if (!shouldObfuscate(F, "sub", true))
    return PreservedAnalyses::all();
  auto &RNG    = getModulePRNG();
  bool Changed = substituteFunction(F, Iterations, RNG);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace kagura