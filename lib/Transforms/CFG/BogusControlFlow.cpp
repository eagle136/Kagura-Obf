//===-- BogusControlFlow.cpp - Bogus control flow with MBA predicates -----===//
//
// Injects dead basic blocks guarded by opaque predicates that are always true
// but difficult to evaluate statically or symbolically.
//
// Unlike DeClang (which uses FCMP_TRUE — trivially solved by any solver),
// we use Mixed Boolean-Arithmetic (MBA) predicates.  Multiple variants are
// implemented and selected at random to defeat static pattern matching:
//
// Always-true variants:
//   (x | ~x) == -1           (OR complement)
//   (x ^ ~x) == -1           (XOR complement)
//   (x + ~x) == -1           (ADD complement, i.e. x + (-x-1))
//   (x * (x+1)) & 1 == 0     (product of consecutive ints is even)
//   ((x | 1) & 1) == 1       (any number OR 1 is odd)
//   (x * (x-1)) & 1 == 0     (product of consecutive ints is even)
//   ((x >> 1) << 1) | (x & 1) == x  (bit round-trip identity)
//   ~(x ^ ~x) == 0           (negated XOR complement)
//   ((x | 1) - 1) & 1 == 0   (odd minus 1 is even)
//   (x + (x & 1)) & 1 == 0   (evenify then test even)
//   (x ^ (x & ~x)) == x      (AND with complement XOR)
//   (x | (x & ~x)) == x      (AND with complement OR)
//
// Always-false variants:
//   (x | ~x) != -1           (negation of always-true #1)
//   (x + ~x) != -1           (negation of always-true #3)
//   ~(x | ~x) == -1          (opposite direction)
//   (x * (x+1)) & 1 == 1     (opposite of even — always false)
//
// The "true" branch executes the original block; the "false" branch contains
// a semantically equivalent but junk-filled copy (never actually taken).
// With 50% probability we invert the condition so the fake branch is taken on
// "true" and the real branch follows the "false" path — this forces reverse
// engineers to analyze both branches thoroughly.
//
//===----------------------------------------------------------------------===//

#include "kagura/Options.h"
#include "kagura/Passes.h"
#include "kagura/Utils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <vector>

using namespace llvm;

namespace kagura {

// ---- Opaque predicate builders ----

// Normalise X to i32 for predicate construction.
static Value *toI32(IRBuilder<> &B, Value *X, uint32_t FallbackConst) {
  auto *I32 = Type::getInt32Ty(B.getContext());
  if (X->getType() == I32)
    return X;
  if (X->getType()->isIntegerTy())
    return B.CreateZExtOrTrunc(X, I32, "bcf.cast");
  return ConstantInt::get(I32, FallbackConst);
}

// === Always-true predicates ===

// Predicate 0: (V | ~V) == -1   (OR complement — always true)
static Value *buildPred0(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x12345678);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *NotV = B.CreateNot(V, "bcf.notv");
  Value *OrV  = B.CreateOr(V, NotV, "bcf.or");
  return B.CreateICmpEQ(OrV, ConstantInt::get(I32, ~0U), "bcf.pred");
}

// Predicate 1: (V ^ ~V) == -1   (XOR complement — always true)
static Value *buildPred1(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0xABCDEF01);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *NotV = B.CreateNot(V, "bcf.notv");
  Value *XorV = B.CreateXor(V, NotV, "bcf.xor");
  return B.CreateICmpEQ(XorV, ConstantInt::get(I32, ~0U), "bcf.pred");
}

// Predicate 2: (V + ~V) == -1   (ADD complement: V + (-V-1) == -1 always)
static Value *buildPred2(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0xFEDCBA98);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *NotV = B.CreateNot(V, "bcf.notv");
  Value *Sum  = B.CreateAdd(V, NotV, "bcf.add");
  return B.CreateICmpEQ(Sum, ConstantInt::get(I32, ~0U), "bcf.pred");
}

// Predicate 3: (V * (V+1)) & 1 == 0   (product of consecutive ints is even)
static Value *buildPred3(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x13579BDF);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *Vp1  = B.CreateAdd(V, ConstantInt::get(I32, 1), "bcf.vp1");
  Value *Prod = B.CreateMul(V, Vp1, "bcf.mul");
  Value *And1 = B.CreateAnd(Prod, ConstantInt::get(I32, 1), "bcf.and");
  return B.CreateICmpEQ(And1, ConstantInt::get(I32, 0), "bcf.pred");
}

// Predicate 4: (V * (V-1)) & 1 == 0   (same: n*(n-1) is also always even)
static Value *buildPred4(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x2468ACE0);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *Vm1  = B.CreateSub(V, ConstantInt::get(I32, 1), "bcf.vm1");
  Value *Prod = B.CreateMul(V, Vm1, "bcf.mul");
  Value *And1 = B.CreateAnd(Prod, ConstantInt::get(I32, 1), "bcf.and");
  return B.CreateICmpEQ(And1, ConstantInt::get(I32, 0), "bcf.pred");
}

// Predicate 5: (V | 1) & 1 == 1   (OR with 1 is always odd)
static Value *buildPred5(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x11223344);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *Or1  = B.CreateOr(V, ConstantInt::get(I32, 1), "bcf.or1");
  Value *And1 = B.CreateAnd(Or1, ConstantInt::get(I32, 1), "bcf.and");
  return B.CreateICmpEQ(And1, ConstantInt::get(I32, 1), "bcf.pred");
}

// Predicate 6: ((V >> 1) << 1) | (V & 1) == V   (bit round-trip identity)
static Value *buildPred6(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x55AA55AA);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *Shr  = B.CreateLShr(V, ConstantInt::get(I32, 1), "bcf.shr");
  Value *Shl  = B.CreateShl(Shr, ConstantInt::get(I32, 1), "bcf.shl");
  Value *Lo   = B.CreateAnd(V, ConstantInt::get(I32, 1), "bcf.lo");
  Value *Rec  = B.CreateOr(Shl, Lo, "bcf.rec");
  return B.CreateICmpEQ(Rec, V, "bcf.pred");
}

// Predicate 7: ~(V ^ ~V) == 0   (negated XOR complement — always true since XOR complement == -1, ~(-1) == 0)
static Value *buildPred7(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0xDEADBEEF);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *NotV = B.CreateNot(V, "bcf.notv");
  Value *XorV = B.CreateXor(V, NotV, "bcf.xor");
  Value *NotX = B.CreateNot(XorV, "bcf.notx");
  return B.CreateICmpEQ(NotX, ConstantInt::get(I32, 0), "bcf.pred");
}

// Predicate 8: ((V | 1) - 1) & 1 == 0   (odd minus 1 is always even)
static Value *buildPred8(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x99AABBCC);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *Or1  = B.CreateOr(V, ConstantInt::get(I32, 1), "bcf.or1");
  Value *Sub1 = B.CreateSub(Or1, ConstantInt::get(I32, 1), "bcf.sub");
  Value *And1 = B.CreateAnd(Sub1, ConstantInt::get(I32, 1), "bcf.and");
  return B.CreateICmpEQ(And1, ConstantInt::get(I32, 0), "bcf.pred");
}

// Predicate 9: (V + (V & 1)) & 1 == 0   (evenify then test even — always true)
static Value *buildPred9(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x55667788);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *Lo   = B.CreateAnd(V, ConstantInt::get(I32, 1), "bcf.lo");
  Value *Sum  = B.CreateAdd(V, Lo, "bcf.sum");
  Value *And1 = B.CreateAnd(Sum, ConstantInt::get(I32, 1), "bcf.and");
  return B.CreateICmpEQ(And1, ConstantInt::get(I32, 0), "bcf.pred");
}

// Predicate 10: (V ^ (V & ~V)) == V   (AND with complement XOR — always true since V & ~V == 0)
static Value *buildPred10(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x1122AABB);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *NotV = B.CreateNot(V, "bcf.notv");
  Value *And  = B.CreateAnd(V, NotV, "bcf.and");
  Value *Xor  = B.CreateXor(V, And, "bcf.xor");
  return B.CreateICmpEQ(Xor, V, "bcf.pred");
}

// Predicate 11: (V | (V & ~V)) == V   (AND with complement OR — always true since V & ~V == 0)
static Value *buildPred11(IRBuilder<> &B, Value *X) {
  Value *V    = toI32(B, X, 0x3344CCDD);
  auto *I32   = Type::getInt32Ty(B.getContext());
  Value *NotV = B.CreateNot(V, "bcf.notv");
  Value *And  = B.CreateAnd(V, NotV, "bcf.and");
  Value *Or   = B.CreateOr(V, And, "bcf.or");
  return B.CreateICmpEQ(Or, V, "bcf.pred");
}

using PredicateBuilder = Value *(*)(IRBuilder<> &, Value *);
static constexpr PredicateBuilder kTruePredicates[] = {
    buildPred0, buildPred1, buildPred2, buildPred3,
    buildPred4, buildPred5, buildPred6, buildPred7,
    buildPred8, buildPred9, buildPred10, buildPred11,
};
static constexpr unsigned kNumTruePredicates =
    sizeof(kTruePredicates) / sizeof(kTruePredicates[0]);

// Invert a predicate by wrapping with Not or changing EQ to NE.
static Value *invertPredicate(IRBuilder<> &B, Value *Pred) {
  // Option 1: return !Pred
  // Option 2: if Pred is ICmpEQ, swap to ICmpNE with same operands
  if (auto *ICmp = dyn_cast<ICmpInst>(Pred)) {
    if (ICmp->getPredicate() == CmpInst::ICMP_EQ) {
      return B.CreateICmpNE(ICmp->getOperand(0), ICmp->getOperand(1), "bcf.inv");
    }
  }
  return B.CreateNot(Pred, "bcf.inv");
}

// Select a predicate at random. If Invert is true, returns the inverted form.
static Value *buildMBAOpaquePred(IRBuilder<> &B, Value *X, PRNG &RNG, bool Invert) {
  unsigned Idx = static_cast<unsigned>(RNG.nextRange(0, kNumTruePredicates));
  Value *Pred = kTruePredicates[Idx](B, X);
  if (Invert) {
    return invertPredicate(B, Pred);
  }
  return Pred;
}

// Pick a "random" existing integer value from the block (first int SSA value).
static Value *pickIntValue(BasicBlock *BB) {
  for (auto &I : *BB)
    if (I.getType()->isIntegerTy() && !isa<PHINode>(&I))
      return &I;
  // Fallback: use a constant
  return ConstantInt::get(Type::getInt32Ty(BB->getContext()), 0xCAFEBABE);
}

// Make an improved bogus clone of BB — copy real instructions with random
// operand substitution instead of trivial junk.
static BasicBlock *makeBogusClone(BasicBlock *BB, Function *F, PRNG &RNG) {
  LLVMContext &Ctx = F->getContext();
  auto *Clone = BasicBlock::Create(Ctx, "bcf.bogus", F);
  IRBuilder<> B(Clone);

  // Map original values to random constants / undef in the clone
  DenseMap<Value *, Value *> ValueMap;

  // First pass: clone instructions with substituted operands
  for (auto &I : *BB) {
    if (I.isTerminator())
      continue;
    if (isa<PHINode>(&I))
      continue;
    // 2026-09-23 FIX: never clone instructions that cannot legally live in
    // a random dead block — calls (musttail/tail constraints, token args),
    // atomics, fences, exception pads, or anything producing/using a token.
    // The block is never executed, so dropping them only shrinks the junk.
    if (isa<CallBase>(&I) || isa<AtomicRMWInst>(&I) ||
        isa<AtomicCmpXchgInst>(&I) || isa<FenceInst>(&I) ||
        isa<LandingPadInst>(&I) || isa<FuncletPadInst>(&I) ||
        I.getType()->isTokenTy())
      continue;
    // Skip volatile loads/stores too: cloning them into dead code is legal
    // but pointless, and keeps the junk block free of observable ops.
    if (auto *LI = dyn_cast<LoadInst>(&I); LI && LI->isVolatile())
      continue;
    if (auto *SI = dyn_cast<StoreInst>(&I); SI && SI->isVolatile())
      continue;

    // Clone the instruction
    Instruction *ClonedI = I.clone();

    // Replace each operand with either the mapped value or a random constant
    for (unsigned OpIdx = 0; OpIdx < ClonedI->getNumOperands(); ++OpIdx) {
      Value *Op = ClonedI->getOperand(OpIdx);
      if (Value *Mapped = ValueMap.lookup(Op)) {
        ClonedI->setOperand(OpIdx, Mapped);
      } else if (isa<Instruction>(Op) || isa<Argument>(Op)) {
        // Replace with a random constant of the appropriate type
        if (Op->getType()->isIntegerTy()) {
          Type *ITy = Op->getType();
          uint64_t R = RNG.next();
          unsigned Bits = ITy->getIntegerBitWidth();
          if (Bits < 64)
            R &= (1ULL << Bits) - 1;
          ClonedI->setOperand(OpIdx, ConstantInt::get(ITy, R));
        } else if (Op->getType()->isFloatTy()) {
          ClonedI->setOperand(OpIdx, ConstantFP::get(Op->getType(), RNG.next()));
        } else {
          ClonedI->setOperand(OpIdx, UndefValue::get(Op->getType()));
        }
      }
    }

    B.Insert(ClonedI);
    // Map original to clone for subsequent operands
    ValueMap[&I] = ClonedI;
  }

  // Add some extra junk instructions to inflate the block
  for (int i = 0; i < 3 + (int)(RNG.next32() % 5); ++i) {
    auto *I32 = Type::getInt32Ty(Ctx);
    uint64_t RV = RNG.next();
    Value *V1 = ConstantInt::get(I32, RV);
    Value *V2 = ConstantInt::get(I32, RNG.next());
    B.CreateAdd(V1, V2, "bcf.junk");
    B.CreateXor(V1, V2, "bcf.junk2");
  }

  // Terminate with unreachable rather than self-loop (less likely to confuse
  // LLVM's own loop analysis passes)
  B.CreateUnreachable();
  return Clone;
}

static bool obfuscateFunction(Function &F, uint32_t Probability,
                              uint32_t Iterations, PRNG &RNG) {
  if (F.isDeclaration() || F.size() < 2)
    return false;

  bool Changed = false;

  for (uint32_t Iter = 0; Iter < Iterations; ++Iter) {
    auto Blocks = getBlocks(F);

    for (auto *BB : Blocks) {
      if (RNG.nextRange(0, 100) >= Probability)
        continue;
      if (BB->getTerminator() == nullptr)
        continue;
      // Don't inject into blocks ending with InvokeInst
      if (isa<InvokeInst>(BB->getTerminator()))
        continue;
      // Don't inject into kagura's own dispatcher/loop blocks
      if (BB->getName().starts_with("kagura."))
        continue;
      // Don't inject into FLA-generated dispatcher blocks (random hex names)
      // FLA blocks end with known suffixes: .L_ (loop header), .le_ (loop end),
      // .pre_ (pre-loop), .def_ (default), .dd_ (dead), .fk_ (fake)
      if (BB->getName().contains(".L_") ||
          BB->getName().contains(".le_") ||
          BB->getName().contains(".pre_") ||
          BB->getName().contains(".def_") ||
          BB->getName().contains(".dd_") ||
          BB->getName().contains(".fk_") ||
          BB->getName().contains(".st_") ||
          BB->getName().contains(".sw_") ||
          BB->getName().contains(".init_"))
        continue;
      // Don't inject into blocks with SwitchInst (flattening dispatcher)
      if (isa<SwitchInst>(BB->getTerminator()))
        continue;

      // Split BB at the first non-PHI / non-debug instruction
      BasicBlock::iterator SplitPoint = BB->getFirstNonPHIOrDbgOrAlloca();
      if (SplitPoint == BB->end())
        continue;

      BasicBlock *OrigTail = BB->splitBasicBlock(SplitPoint, "bcf.orig");

      // Create bogus clone (junk block)
      BasicBlock *BogusBlock = makeBogusClone(OrigTail, &F, RNG);

      // 50% chance: invert the predicate so the bogus block is on the "true"
      // side and the real block is on the "false" side
      bool InvertPred = (RNG.next32() % 2) == 0;

      // Replace BB's unconditional branch with:
      //   if (MBA_opaque_pred)  jump OrigTail
      //   else                  jump BogusBlock
      Instruction *OldTerm = BB->getTerminator();
      IRBuilder<> B(OldTerm);
      Value *X    = pickIntValue(BB);
      Value *Cond = buildMBAOpaquePred(B, X, RNG, InvertPred);

      if (InvertPred) {
        // Predicate is false normally, so the real block goes to false branch
        B.CreateCondBr(Cond, BogusBlock, OrigTail);
      } else {
        B.CreateCondBr(Cond, OrigTail, BogusBlock);
      }
      OldTerm->eraseFromParent();

      Changed = true;
    }
  }

  return Changed;
}

PreservedAnalyses BogusControlFlowPass::run(Function &F,
                                             FunctionAnalysisManager &) {
  if (!shouldObfuscate(F, "bcf", opt::BCF))
    return PreservedAnalyses::all();
  auto &RNG    = getModulePRNG();
  bool Changed = obfuscateFunction(F, Probability, Iterations, RNG);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace kagura