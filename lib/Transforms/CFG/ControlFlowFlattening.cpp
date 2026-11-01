//===-- ControlFlowFlattening.cpp - CFG flattening pass (enhanced) ---------===//
//
// Converts function control flow into a switch-based state machine.
// Uses New Pass Manager API (PassInfoMixin).
//
// Enhancements over base Kagura FLA:
//   1. XOR-encrypted switch variable — state is stored as (realVal ^ Key)
//   2. MBA-encoded case values — each case constant is derived from a base
//      offset plus MBA expression to disguise the dispatcher structure
//   3. Random block names — no more "kagura.loop" naming that reveals
//      the flattened structure
//   4. Fake case branches — 3-5 dead switch entries to increase CFG
//      analysis complexity
//   5. Case value 0 is now allowed (no longer skipped), making analysis
//      harder since 0 is no longer a sentinel
//
// 2026-09-23 correctness fixes:
//   F1. Entry block handling.  The old code only understood a BranchInst
//       entry terminator and degraded the initial state to 0 when the entry
//       ended with a SwitchInst -> dispatcher fell into DefaultBB ->
//       unreachable -> hang.  Now the entry block is SPLIT at its
//       terminator: OrigEntry keeps only the state alloca + a bootstrap
//       store that always dispatches to EntryBody; the original terminator
//       moves to EntryBody which joins the state machine as a normal node.
//       This makes the entry-terminator shape (branch OR switch) uniform,
//       and LLVM's "entry must not have predecessors" rule stays satisfied.
//   F2. Blocks terminated by a SwitchInst (including EntryBody) are
//       rewritten: each case/default destination is redirected to a stub
//       block that stores the destination's state and branches to the
//       dispatcher loop.  Semantics preserved exactly.
//   F3. Fake case values are deduplicated against real AND other fake
//       values (duplicate switch entries are invalid IR).
//   F4. All real case values are deduplicated (random collision would
//       produce invalid IR).
//   F5. If a successor has no case value (e.g. the original entry block,
//       which may not be a dispatch target), the block is left unchanged
//       instead of degrading to state 0 — control still flows correctly
//       through the direct branch.
//
//===----------------------------------------------------------------------===//

#include "kagura/Passes.h"
#include "kagura/Utils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace kagura {

// Generate a random hex suffix for block names to avoid revealing structure
static std::string randomBlockSuffix(PRNG &RNG, const char *Prefix) {
  uint32_t R = RNG.next32();
  char Buf[32];
  snprintf(Buf, sizeof(Buf), "%s%08X", Prefix, R);
  return std::string(Buf);
}

// Build a simple MBA opaque predicate that is ALWAYS true:
//   (V | ~V) == -1  — always true
// Used for guarding fake branches (the fake branch is never taken
// because we invert the condition later or route through dead code).
static Value *buildMBAOpaqueTrue(IRBuilder<> &B, Value *X, PRNG &RNG) {
  // Pick from several variants
  uint32_t Choice = RNG.next32() % 3;
  auto *I32 = Type::getInt32Ty(B.getContext());

  // Normalise X to i32
  Value *V = X;
  if (V->getType() != I32) {
    if (V->getType()->isIntegerTy())
      V = B.CreateZExtOrTrunc(V, I32, "fla.cast");
    else
      V = ConstantInt::get(I32, 0x12345678);
  }

  switch (Choice) {
  case 0: {
    // (V | ~V) == -1
    Value *NotV = B.CreateNot(V, "fla.notv");
    Value *OrV  = B.CreateOr(V, NotV, "fla.or");
    return B.CreateICmpEQ(OrV, ConstantInt::get(I32, ~0U), "fla.pred");
  }
  case 1: {
    // (V ^ ~V) == -1
    Value *NotV = B.CreateNot(V, "fla.notv");
    Value *XorV = B.CreateXor(V, NotV, "fla.xor");
    return B.CreateICmpEQ(XorV, ConstantInt::get(I32, ~0U), "fla.pred");
  }
  case 2: {
    // (V + ~V) == -1
    Value *NotV = B.CreateNot(V, "fla.notv");
    Value *Sum  = B.CreateAdd(V, NotV, "fla.add");
    return B.CreateICmpEQ(Sum, ConstantInt::get(I32, ~0U), "fla.pred");
  }
  }
  // Fallback
  Value *NotV = B.CreateNot(V, "fla.notv");
  Value *OrV  = B.CreateOr(V, NotV, "fla.or");
  return B.CreateICmpEQ(OrV, ConstantInt::get(I32, ~0U), "fla.pred");
}

static bool flattenFunction(Function &F, PRNG &RNG) {
  if (F.isDeclaration() || F.size() < 3)
    return false;

  // Only handle terminators we know how to rewrite.  Anything exotic
  // (resume / catchswitch / callbr / ...) bails out of flattening entirely
  // so we never produce a half-rewired CFG.
  for (auto &BB : F) {
    Instruction *Term = BB.getTerminator();
    if (isa<IndirectBrInst>(Term) || isa<InvokeInst>(Term) ||
        isa<ResumeInst>(Term) || isa<CatchSwitchInst>(Term) ||
        isa<CatchReturnInst>(Term) || isa<CleanupReturnInst>(Term) ||
        isa<CallBrInst>(Term))
      return false;
  }
  BasicBlock *OrigEntry = &F.getEntryBlock();
  Instruction *EntryTerm = OrigEntry->getTerminator();
  if (!isa<BranchInst>(EntryTerm) && !isa<SwitchInst>(EntryTerm))
    return false; // entry must have dispatchable successors

  LLVMContext &Ctx = F.getContext();
  auto *Int32Ty    = Type::getInt32Ty(Ctx);

  // Step 1: Demote PHI nodes to memory
  demotePhis(F);

  // Step 2 (FIX F1): split the entry block at its terminator.
  //   OrigEntry : original instructions + bootstrap store + br PreLoop
  //   EntryBody : the ORIGINAL entry terminator, participating in the
  //               state machine as a normal node.
  // The branch/switch decision therefore always happens with the original
  // condition values, and the entry block never gains predecessors.
  EntryTerm = OrigEntry->getTerminator();
  BasicBlock *EntryBody =
      OrigEntry->splitBasicBlock(EntryTerm->getIterator(),
                                 Twine(randomBlockSuffix(RNG, ".eb_")));

  std::vector<BasicBlock *> WorkBlocks;
  for (auto &BB : F)
    if (&BB != OrigEntry)
      WorkBlocks.push_back(&BB);
  if (WorkBlocks.empty())
    return false;

  // Step 3: Generate XOR encryption key for this function's switch variable
  uint32_t SwitchKey = RNG.next32();
  if (SwitchKey == 0) SwitchKey = 0xAAAAAAAA; // ensure non-zero

  // Step 4: Assign case values to every dispatchable block.
  // FIX F4: values are deduplicated — duplicate switch entries are invalid IR.
  std::map<BasicBlock *, uint32_t> CaseMap;
  {
    std::set<uint32_t> Used;
    for (auto *BB : WorkBlocks) {
      uint32_t Val;
      do {
        Val = RNG.next32();
      } while (!Used.insert(Val).second);
      CaseMap[BB] = Val;
    }
  }

  // Step 5: Insert alloca for the switch variable AT THE TOP of OrigEntry
  IRBuilder<> AllocaBuilder(&*OrigEntry->getFirstInsertionPt());
  auto *SwitchVar = AllocaBuilder.CreateAlloca(Int32Ty, nullptr,
      Twine(randomBlockSuffix(RNG, ".sw_")));

  // Step 6: Create the dispatcher blocks with random names
  BasicBlock *PreLoop = BasicBlock::Create(Ctx,
      Twine(randomBlockSuffix(RNG, ".pre_")), &F);
  BasicBlock *LoopHeader = BasicBlock::Create(Ctx,
      Twine(randomBlockSuffix(RNG, ".L_")), &F);
  BasicBlock *LoopEnd = BasicBlock::Create(Ctx,
      Twine(randomBlockSuffix(RNG, ".le_")), &F);
  BasicBlock *DefaultBB = BasicBlock::Create(Ctx,
      Twine(randomBlockSuffix(RNG, ".def_")), &F);

  // Default block: create an opaque false predicate that leads to a dead loop
  // This hides the fact that DefaultBB is never reached.
  {
    IRBuilder<> DB(DefaultBB);
    Value *SomeVal = ConstantInt::get(Int32Ty, RNG.next32());
    Value *NotV = DB.CreateNot(SomeVal, "fla.dnot");
    Value *OrV  = DB.CreateOr(SomeVal, NotV, "fla.dor");
    Value *Cond = DB.CreateICmpNE(OrV, ConstantInt::get(Int32Ty, ~0U), "fla.dpred");
    auto *DeadBB = BasicBlock::Create(Ctx,
        Twine(randomBlockSuffix(RNG, ".dd_")), &F);
    IRBuilder<>(DeadBB).CreateUnreachable();
    DB.CreateCondBr(Cond, DeadBB, DeadBB); // never branches to anything useful
  }

  // Store (encrypted) state into the switch variable.
  auto StoreState = [&](IRBuilder<> &B, Value *StateVal) {
    Value *Enc = B.CreateXor(StateVal, ConstantInt::get(Int32Ty, SwitchKey),
                             "fla.xor");
    B.CreateStore(Enc, SwitchVar);
  };

  // FIX F2: rewrite a SwitchInst in place.  Every successor (including the
  // default destination) is redirected to a fresh stub block that stores
  // that destination's case value and branches to LoopEnd.  The switch
  // condition is kept, so the original branch decision is preserved.
  auto RewriteSwitch = [&](SwitchInst *SI) {
    for (unsigned i = 0, e = SI->getNumSuccessors(); i < e; ++i) {
      BasicBlock *Dest = SI->getSuccessor(i);
      if (!CaseMap.count(Dest))
        continue; // FIX F5: leave untouched instead of inventing value 0
      auto *Stub = BasicBlock::Create(Ctx,
          Twine(randomBlockSuffix(RNG, ".st_")), &F);
      IRBuilder<> SB(Stub);
      StoreState(SB, ConstantInt::get(Int32Ty, CaseMap[Dest]));
      SB.CreateBr(LoopEnd);
      SI->setSuccessor(i, Stub);
    }
  };

  // Step 7: Bootstrap.  OrigEntry currently ends with `br EntryBody` (from
  // the split).  Replace it with: store state(EntryBody) + br PreLoop.
  // EntryBody's own terminator (the original one) then performs the real
  // first branch decision — uniform for branch- and switch-shaped entries.
  {
    OrigEntry->getTerminator()->eraseFromParent();
    IRBuilder<> B(OrigEntry);
    StoreState(B, ConstantInt::get(Int32Ty, CaseMap[EntryBody]));
    B.CreateBr(PreLoop);
  }

  // PreLoop -> LoopHeader (simple branch)
  IRBuilder<>(PreLoop).CreateBr(LoopHeader);

  // Step 8: Build LoopHeader — load, XOR-decrypt, branch via switch
  {
    IRBuilder<> LB(LoopHeader);
    auto *EncSW = LB.CreateLoad(Int32Ty, SwitchVar, "fla.load");
    auto *SW = LB.CreateXor(EncSW, ConstantInt::get(Int32Ty, SwitchKey),
                            "fla.dec");
    auto *Switch = LB.CreateSwitch(SW, DefaultBB, CaseMap.size());

    // Add real case branches.
    // 2026-10-08 (K5): iterate WorkBlocks (block order) instead of CaseMap.
    // CaseMap is keyed by BasicBlock* so its iteration order follows heap
    // addresses, which change with ASLR on every opt run -> the switch case
    // order (and therefore the emitted code) differed between two builds of
    // the same source and same seed, breaking reproducible builds.
    std::set<uint32_t> UsedCaseVals;
    for (auto *BB : WorkBlocks) {
      uint32_t Val = CaseMap[BB];
      Switch->addCase(ConstantInt::get(Int32Ty, Val), BB);
      UsedCaseVals.insert(Val);
    }

    // Step 8b: Add fake case branches (3-5 extra entries)
    // FIX F3: deduplicate against real AND previously added fake values —
    // duplicate case values are invalid IR.
    unsigned NumFake = 3 + (RNG.next32() % 3); // 3..5
    for (unsigned i = 0; i < NumFake; ++i) {
      uint32_t FakeVal;
      do {
        FakeVal = RNG.next32();
      } while (UsedCaseVals.count(FakeVal));
      UsedCaseVals.insert(FakeVal);

      auto *FakeBB = BasicBlock::Create(Ctx,
          Twine(randomBlockSuffix(RNG, ".fk_")), &F);
      IRBuilder<> FB(FakeBB);
      auto *FakeLoad = FB.CreateLoad(Int32Ty, SwitchVar, "fla.fload");
      auto *FakeXor  = FB.CreateXor(FakeLoad,
                                    ConstantInt::get(Int32Ty, SwitchKey),
                                    "fla.fxor");
      auto *FakeSwitch = FB.CreateSwitch(FakeXor, DefaultBB, 1);
      FakeSwitch->addCase(ConstantInt::get(Int32Ty, FakeVal), LoopEnd);

      Switch->addCase(ConstantInt::get(Int32Ty, FakeVal), FakeBB);
    }
  }

  // LoopEnd -> LoopHeader
  IRBuilder<>(LoopEnd).CreateBr(LoopHeader);

  // Step 9: Wire each work block into the switch
  for (auto *BB : WorkBlocks) {
    Instruction *Term = BB->getTerminator();
    IRBuilder<> TB(Term);

    if (isa<ReturnInst>(Term) || isa<UnreachableInst>(Term))
      continue;

    if (auto *SI = dyn_cast<SwitchInst>(Term)) {
      // FIX F2: work-block switches (including EntryBody's) get the same
      // stub treatment, so their successors rejoin the dispatcher with the
      // correct state.
      RewriteSwitch(SI);
      continue;
    }

    if (auto *Br = dyn_cast<BranchInst>(Term)) {
      if (Br->isUnconditional()) {
        BasicBlock *Succ = Br->getSuccessor(0);
        if (!CaseMap.count(Succ))
          continue; // FIX F5: leave unchanged rather than degrade to state 0
        StoreState(TB, ConstantInt::get(Int32Ty, CaseMap[Succ]));
        Br->eraseFromParent();
        IRBuilder<>(BB).CreateBr(LoopEnd);
      } else {
        BasicBlock *TBB = Br->getSuccessor(0);
        BasicBlock *FBB = Br->getSuccessor(1);
        if (!CaseMap.count(TBB) || !CaseMap.count(FBB))
          continue; // FIX F5
        Value *Cond = Br->getCondition();
        auto *Sel   = TB.CreateSelect(Cond,
                                      ConstantInt::get(Int32Ty, CaseMap[TBB]),
                                      ConstantInt::get(Int32Ty, CaseMap[FBB]),
                                      Twine(randomBlockSuffix(RNG, ".sel_")));
        StoreState(TB, Sel);
        Br->eraseFromParent();
        IRBuilder<>(BB).CreateBr(LoopEnd);
      }
      continue;
    }
    // Other terminators: leave unchanged
  }

  return true;
}

PreservedAnalyses
ControlFlowFlatteningPass::run(Function &F, FunctionAnalysisManager &) {
  // C.1: FLA rewires the entire CFG into a switch dispatch and relies on
  // reg2mem (DemotePHIToStack / DemoteRegToStack).  The Wasm target requires
  // structured control flow and does not support arbitrary CFG reshaping.
  if (kagura::isWasmTarget(*F.getParent()))
    return PreservedAnalyses::all();

  if (!shouldObfuscate(F, "fla", true))
    return PreservedAnalyses::all();
  auto &RNG    = getModulePRNG();
  bool Changed = flattenFunction(F, RNG);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace kagura
