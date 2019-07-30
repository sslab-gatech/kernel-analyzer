#include <llvm/Pass.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/TypeFinder.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringMap.h>

#include <map>
#include <queue>
#include <vector>
#include <sys/types.h>
#include <iostream>
#include <fstream>

#include "LinuxSS.h"
#include "Annotation.h"

using namespace llvm;

#define LSS_DEBUG(stmt) KA_LOG(2, stmt)

#define LSS_LOG(stmt)        \
    do {                    \
        LSS_DEBUG(stmt);    \
        outs() << stmt;        \
    } while (0)

#define MEPERM    2311
#define MEACCES   23113
#define MEROFS    23130

static void insertControlBlock(BasicBlock *BB, LinuxSS::BBSet &BS) {

    if (BranchInst *BR = dyn_cast<BranchInst>(BB->getTerminator())) {
        if (!BR->isConditional()) {
            if (BasicBlock *PB = BB->getSinglePredecessor()) {
                BS.insert(PB);
                return;
            }
        }
    }

    // insert bb in all other cases
    BS.insert(BB);
}

bool LinuxSS::collectCondition(Value *V) {
    std::set<llvm::Value*> *C = reinterpret_cast<std::set<llvm::Value*>*>(Ctx->get("SecConds"));
    return C->insert(V).second;
}

bool LinuxSS::dumpControlDep(BasicBlock *CBB, BBSet &Checked) {

    bool ret = false;

    BBSet Visited;
    SmallVector<BasicBlock*, 8> WorkList;

    LSS_DEBUG("=== Checked = ");
    for (BasicBlock *BB : Checked) {
        BB->printAsOperand(errs());
        LSS_DEBUG(", ");
    }
    LSS_DEBUG("\n");

    Visited.insert(CBB);
    WorkList.push_back(CBB);
    while (!WorkList.empty()) {
        BasicBlock *BB = WorkList.pop_back_val();

        if (VerboseLevel >= 2) {
            errs() << "Check BB: ";
            BB->printAsOperand(errs());
            errs() << "\n";
        }

        // ignore checked/blacklisted
        // unless the input block
        if (BB != CBB && !Checked.insert(BB).second) {
            // check its predecessor(s) insteand
            for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI) {
                if (Visited.insert(*PI).second)
                    WorkList.push_back(*PI);
            }
            continue;
        }

        Instruction *TI = BB->getTerminator();
        if (BranchInst *BI = dyn_cast<BranchInst>(TI)) {
            if (BI->isConditional()) {
                Value *Cond = BI->getCondition();
                ret |= collectCondition(Cond);
            } else {
                for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI) {
                    if (Visited.insert(*PI).second)
                        WorkList.push_back(*PI);
                }
            }
        } else if (SwitchInst *SI = dyn_cast<SwitchInst>(TI)) {
            Value *Cond = SI->getCondition();
            ret |= collectCondition(Cond);
        }
    }

    return ret;
}

bool LinuxSS::isTrueFalseFunc(Function *F) {

    // check return type, if declared as true/false func
    Type *RTy = F->getReturnType();
    IntegerType *ITy = dyn_cast<IntegerType>(RTy);
    if (ITy && ITy->getBitWidth() == 1)
        return true;

    // collect return values
    ValueSet Visited;
    RetSet RS;
    for (BasicBlock &BB : *F) {
        Instruction *T = BB.getTerminator();
        if (ReturnInst *R = dyn_cast<ReturnInst>(T)) {
            Value *RV = R->getReturnValue();
            if (RV != nullptr)
                collectRetVal(RV, &BB, RS, Visited);
        }
    }

    for (RetPair const& RP : RS) {
        Value *V = RP.first;
        Type *Ty = V->getType();

        if (!Ty->isIntegerTy())
            return false;

        if (Ty->isIntegerTy(1))
            return true;

        if (ConstantInt *Int = dyn_cast<ConstantInt>(V)) {
            if (Int->getSExtValue() > 0)
                return true;
        }
    }

    return false;
}

bool LinuxSS::isErrorBranch(BasicBlock *Ancestor, BasicBlock *Descendent) {

    BBSet Visited;
    SmallVector<BasicBlock*, 8> WorkList;

    Visited.insert(Descendent);
    WorkList.push_back(Descendent);
    while (!WorkList.empty()) {
        BasicBlock *BB = WorkList.pop_back_val();

        for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI) {
            BasicBlock *P = *PI;
            if (P == Ancestor) {
                // let's be conservative about this
                // only return false when it's a conditional branch
                // that compares return value with 0
                BranchInst *BI = dyn_cast<BranchInst>(Ancestor->getTerminator());
                if (!BI)
                    return true;

                if (!BI->isConditional())
                    return false;

                BasicBlock *TB = BI->getSuccessor(0);
                BasicBlock *FB = BI->getSuccessor(1);

                // check condition
                Value *Cond = BI->getCondition();
                if (CallInst *CI = dyn_cast<CallInst>(Cond)) {
                    Function *F = CI->getCalledFunction();
                    if (F && isTrueFalseFunc(F)) {
                        if (FB == BB) // ret == 0
                            return true;
                        else
                            return false;
                    } else {
                        if (TB == BB) // ret != 0
                            return true;
                        else
                            return false;
                    }
                }

                ICmpInst *Cmp = dyn_cast<ICmpInst>(Cond);
                if (!Cmp)
                    return true;

                Value *op0 = Cmp->getOperand(0);
                Value *op1 = Cmp->getOperand(1);
                Value *NonZero = nullptr;
                Value *Zero = nullptr;
                bool isTrueFalse = false;
                if (ConstantInt *Int = dyn_cast<ConstantInt>(op0)) {
                    if (Int->getZExtValue() == 0) {
                        Zero = op0;
                        NonZero = op1;
                    }
                }
                if (ConstantInt *Int = dyn_cast<ConstantInt>(op1)) {
                    if (Zero != nullptr) {
                        errs() << "Comparing two constant does not make sense" << *Cmp;
                        return false;
                    }

                    if (Int->getZExtValue() == 0) {
                        Zero = op1;
                        NonZero = op0;
                    }
                }
                // neither value is zero, assume true
                if (!Zero)
                    return true;

                if (CastInst *CI = dyn_cast<CastInst>(NonZero))
                    NonZero = CI->getOperand(0);

                if (!isa<CallInst>(NonZero))
                    return true;

                Function *F = cast<CallInst>(NonZero)->getCalledFunction();
                if (!F)
                    return true;

                if (isTrueFalseFunc(F))
                    isTrueFalse = true;

                CmpInst::Predicate Pred = Cmp->getPredicate();
                switch (Pred) {
                case CmpInst::ICMP_EQ:
                    // == 0
                    if (isTrueFalse) {
                        if (TB == BB)
                            return true;
                        else
                            return false;
                    } else {
                        if (FB == BB) // false branch
                            return true;
                        else
                            return false;
                    }

                case CmpInst::ICMP_NE:
                    // != 0
                    if (isTrueFalse) {
                        if (FB == BB)
                            return true;
                        else
                            return false;
                    } else {
                        if (TB == BB) // true branch
                            return true;
                        else
                            return false;
                    }

                case CmpInst::ICMP_SLT:
                    if (op1 == Zero) {
                        // < 0
                        if (TB == BB)
                            return true;
                        else
                            return false;
                    } else {
                        // 0 < ?
                        return true;
                    }

                case CmpInst::ICMP_SGE:
                    if (op1 == Zero) {
                        // >= 0
                        if (FB == BB)
                            return true;
                        else
                            return false;
                    } else {
                        // 0 >= ?
                        return true;
                    }
                default:
                    return true;
                }
            } else { // P != Ancestor
                if (Visited.insert(P).second)
                    WorkList.push_back(P);
            }
        }
    }

    // should neve reach here
    llvm_unreachable("Ancestor should always be reachable from descendent");
}

bool LinuxSS::checkControlDep(BBSet &CheckList, BBSet &BlackList) {

    bool ret = false;

    if (CheckList.empty())
        return false;

    Function *F = (*CheckList.begin())->getParent();

    // ignore bb that is post-dominated by any check
    BBSet Checked;
    for (BasicBlock *BB : CheckList) {
        for (BasicBlock &B : *F) {
            if (gPDT->dominates(BB, &B)) {
                Checked.insert(&B);
            }
        }
    }
    // including those in blacklist
    for (BasicBlock *BB : BlackList) {
        Checked.insert(BB);
        for (BasicBlock &B : *F) {
            if (gPDT->dominates(BB, &B)) {
                Checked.insert(&B);
            }
        }
    }

    for (BasicBlock *BB : CheckList) {
        // check current BB
        ret |= dumpControlDep(BB, Checked);
        Checked.insert(BB);

        // check bb that dominates current bb
        for (BasicBlock &B : *F) {
            if (gDT->dominates(&B, BB)) {
                if (Checked.count(&B))
                    continue;

                // dominator condition may have two cases
                // 1) if the condition is an error, directly return, e.g.,
                //      rc = check_perm();
                //      if (rc)
                //        return rc;
                //    this is not the case we're interested in
                // 2) if the condition is an error, the fall back to another check, e.g.,
                //      if (uid == cred->euid || uid == cred->suid)
                //    this is the case we're interested in
                //
                if (!isErrorBranch(&B, BB))
                    continue;

                ret |= dumpControlDep(&B, Checked);
            }
        }

    }

    return ret;
}

void LinuxSS::collectRetVal(Value *V, BasicBlock *BB, RetSet &RS, ValueSet &Visited) {

    User *U = dyn_cast<User>(V);
    if (U == nullptr)
        return;

    if (ConstantInt *INT = dyn_cast<ConstantInt>(U)) {
        RS.insert(std::make_pair(INT, BB));
        return;
    }

    // always process constant int
    // so check visited here
    if (!Visited.insert(V).second)
        return;
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(U)) {
        if (CE->isCast()) {
            Value *S = CE->getOperand(0);
            collectRetVal(S, BB, RS, Visited);
            return;
        }
        // fall through far other cases
    }

    if (PHINode *PHI = dyn_cast<PHINode>(U)) {
        for (unsigned i = 0, e = PHI->getNumIncomingValues(); i != e; ++i) {
            collectRetVal(PHI->getIncomingValue(i),
                    PHI->getIncomingBlock(i), RS, Visited);
        }
        return;
    }

    if (SelectInst *SI = dyn_cast<SelectInst>(U)) {
        collectRetVal(SI->getTrueValue(), SI->getParent(), RS, Visited);
        collectRetVal(SI->getFalseValue(), SI->getParent(), RS, Visited);
        return;
    }

    if (LoadInst *LI = dyn_cast<LoadInst>(U)) {
        Value *LV = LI->getPointerOperand();
        // check for store operations
        // return values (variables) are supposed to be local,
        // so global alias analysis is not necessary;
        // but what about local ones (FIXME)?
        //
        // FIXME: store -> load -> store -> load
        for (Value::user_iterator UI = LV->user_begin(), UE = LV->user_end();
                 UI != UE; ++UI) {

            if (StoreInst *SI = dyn_cast<StoreInst>(*UI))
                collectRetVal(SI->getValueOperand(),
                        SI->getParent(), RS, Visited);
        }
        return;
    }

    if (CastInst *CI = dyn_cast<CastInst>(U)) {
        Value *S = CI->getOperand(0);
        collectRetVal(S, CI->getParent(), RS, Visited);
        return;
    }

    if (CallInst *CI = dyn_cast<CallInst>(U)) {
        // do not consider forwarded return value
        // except ERR_PTR
        Function *F = CI->getCalledFunction();
        if (F) {
            if (F->hasName() && !F->getName().compare("ERR_PTR")) {
                collectRetVal(CI->getArgOperand(0),
                        CI->getParent(), RS, Visited);
            }
        }
        return;
    }

#if 0
    for (User::op_iterator OI = U->op_begin(), OE = U->op_end();
             OI != OE; ++OI) {
        if (!isa<User>(*OI))
            continue;

        if (Visited.insert(*OI).second)
            WorkList.push_back(*OI);
    }
#else
    LSS_DEBUG("unsupported op: " << *U << "\n");
    RS.insert(std::make_pair(U, BB));
#endif

}

bool LinuxSS::runOnFunction(Function *F) {

    bool ret = false;

    // calculate the dominator tree for current function
    gDT->recalculate(*F);
    gPDT->recalculate(*F);

    ValueSet Visited;
    RetSet RS;
    for (BasicBlock &BB : *F) {
        Instruction *T = BB.getTerminator();
        if (ReturnInst *R = dyn_cast<ReturnInst>(T)) {
            Value *RV = R->getReturnValue();
            if (RV != nullptr)
                collectRetVal(RV, &BB, RS, Visited);
        }
    }

    BBSet CheckList, BlackList;
    for (RetPair const& RP : RS) {
        Value *V = RP.first;
        BasicBlock *BB = RP.second;

        // only consider const int return value
        if (ConstantInt *INT = dyn_cast<ConstantInt>(V)) {
            int64_t i = 0;
            if (INT->getBitWidth() <= 64)
                i = INT->getSExtValue();

            if (i >= 0)
                continue;

            if (i == -MEPERM || i == -MEACCES || i == -MEROFS) {
                LSS_LOG("F: " << getScopeName(F) << "\n");
                insertControlBlock(BB, CheckList);
            } else {
                // branches that lead to unrelated errors are not interested
                insertControlBlock(BB, BlackList);
            }
        }
    }

    ret = checkControlDep(CheckList, BlackList);

    return false;
}

bool LinuxSS::doInitialization(Module *M) {
    return false;
}

bool LinuxSS::doFinalization(Module *M) {
    return false;
}

bool LinuxSS::doModulePass(Module *M) {
    bool changed = true, ret = false;

    while (changed) {
        changed = false;
        for (Function &F : *M) {
            if (F.isIntrinsic() || F.isDeclaration())
                continue;
            changed |= runOnFunction(&F);
        }
        ret |= changed;
    }
    return ret;
}

