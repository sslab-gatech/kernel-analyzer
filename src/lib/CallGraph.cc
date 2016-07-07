/*
 * Call graph construction
 *
 * Copyright (C) 2012 Xi Wang, Haogang Chen, Nickolai Zeldovich
 * Copyright (C) 2015 - 2016 Chengyu Song 
 * Copyright (C) 2016 Kangjie Lu
 *
 * For licensing details see LICENSE
 */


#include <llvm/IR/DebugInfo.h>
#include <llvm/Pass.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Analysis/CallGraph.h>

#include "CallGraph.h"

using namespace llvm;

void CallGraphPass::findCalleesByType(CallInst *CI, FuncSet &S) {
    CallSite CS(CI);
    //errs() << *CI << "\n";
    for (Function *F : Ctx->AddressTakenFuncs) {

        // just compare known args
        if (F->getFunctionType()->isVarArg()) {
            //errs() << "VarArg: " << F->getName() << "\n";
            //report_fatal_error("VarArg address taken function\n");
        } else if (F->arg_size() != CS.arg_size()) {
            //errs() << "ArgNum mismatch: " << F.getName() << "\n";
            continue;
        }

        if (F->isIntrinsic()) {
            //errs() << "Intrinsic: " << F.getName() << "\n";
            continue;
        }

        // type matching on args
        bool Matched = true;
        CallSite::arg_iterator AI = CS.arg_begin();
        for (Function::arg_iterator FI = F->arg_begin(), FE = F->arg_end();
             FI != FE; ++FI, ++AI) {
            // check type mis-match
            Type *FormalTy = FI->getType();
            Type *ActualTy = (*AI)->getType();

            if (FormalTy == ActualTy)
                continue;
            // assume "void *" and "char *" are equivalent to any pointer type
            // and integer type
            else if ((FormalTy == Int8PtrTy &&
                (ActualTy->isPointerTy() || ActualTy == IntPtrTy)) || 
                (ActualTy == Int8PtrTy &&
                (FormalTy->isPointerTy() || FormalTy == IntPtrTy)))
                continue;
            else {
                Matched = false;
                break;
            }
        }

        if (Matched)
            S.insert(F);
    }
}

bool CallGraphPass::findCallees(CallInst *CI, FuncSet &S) {
    Function *CF = CI->getCalledFunction();
    // real function, S = S + {F}
    if (CF) {
        // prefer the real definition to declarations
        FuncMap::iterator it = Ctx->Funcs.find(CF->getName());
        if (it != Ctx->Funcs.end())
            CF = it->second;

        return S.insert(CF).second;
    }

    // save called values for point-to analysis
    Ctx->IndirectCallInsts.push_back(CI);

#ifdef TYPE_BASED
    // use type matching to concervatively find 
    // possible targets of indirect call
    findCalleesByType(CI, FS);
#endif

    return false;
}

bool CallGraphPass::runOnFunction(Function *F) {
    bool Changed = false;

    for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
        // map callsite to possible callees
        if (CallInst *CI = dyn_cast<CallInst>(&*i)) {
            // ignore inline asm or intrinsic calls
            if (CI->isInlineAsm() || (CI->getCalledFunction()
                    && CI->getCalledFunction()->isIntrinsic()))
                continue;

            // might be an indirect call, find all possible callees
            FuncSet &FS = Ctx->Callees[CI];
            findCallees(CI, FS);
        }
    }

    return Changed;
}

bool CallGraphPass::doInitialization(Module *M) {

    DL = &(M->getDataLayout());
    Int8PtrTy = Type::getInt8PtrTy(M->getContext());
    IntPtrTy = DL->getIntPtrType(M->getContext());

    for (Function &F : *M) { 
        // collect address-taken functions
        if (F.hasAddressTaken())
            Ctx->AddressTakenFuncs.insert(&F);
    
        // collect global function definitions
        if (F.hasExternalLinkage() && !F.empty()) {
            // external linkage always ends up with the function name
            StringRef FName = F.getName();
            if (FName.startswith("SyS_"))
                FName = StringRef("sys_" + FName.str().substr(4));
            Ctx->Funcs[FName] = &F;
        }
    }

    return false;
}

bool CallGraphPass::doFinalization(Module *M) {

    // update callee mapping
    for (Function &F : *M) {
        for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
            // map callsite to possible callees
            if (CallInst *CI = dyn_cast<CallInst>(&*i)) {
                FuncSet &FS = Ctx->Callees[CI];
                // calculate the caller info here
                for (Function *CF : FS) {
                    CallInstSet &CIS = Ctx->Callers[CF];
                    CIS.insert(CI);
                }
            }
        }
    }

    return false;
}

bool CallGraphPass::doModulePass(Module *M) {
    bool Changed = true, ret = false;
    while (Changed) {
        Changed = false;
        for (Function &F : *M)
            Changed |= runOnFunction(&F);
        ret |= Changed;
    }
    return ret;
}

// debug
void CallGraphPass::dumpFuncPtrs() {
    raw_ostream &OS = outs();
    for (FuncPtrMap::iterator i = Ctx->FuncPtrs.begin(),
         e = Ctx->FuncPtrs.end(); i != e; ++i) {
        //if (i->second.empty())
        //    continue;
        OS << i->first << "\n";
        FuncSet &v = i->second;
        for (FuncSet::iterator j = v.begin(), ej = v.end();
             j != ej; ++j) {
            OS << "  " << ((*j)->hasInternalLinkage() ? "f" : "F")
                << " " << (*j)->getName().str() << "\n";
        }
    }
}

void CallGraphPass::dumpCallees() {
    RES_REPORT("\n[dumpCallees]\n");
    raw_ostream &OS = outs();
    OS << "Num of Callees: " << Ctx->Callees.size() << "\n";
    for (CalleeMap::iterator i = Ctx->Callees.begin(), 
         e = Ctx->Callees.end(); i != e; ++i) {

        CallInst *CI = i->first;
        FuncSet &v = i->second;
        // only dump indirect call?
        if (CI->isInlineAsm() || CI->getCalledFunction() /*|| v.empty()*/)
             continue;

        OS << "CS:" << *CI << "\n";
        const DebugLoc &LOC = CI->getDebugLoc();
        OS << "LOC: ";
        LOC.print(OS);
        OS << "^@^";
        for (FuncSet::iterator j = v.begin(), ej = v.end();
             j != ej; ++j) {
            //OS << "\t" << ((*j)->hasInternalLinkage() ? "f" : "F")
            //    << " " << (*j)->getName() << "\n";
            OS << (*j)->getName() << "::";
        }
        OS << "\n";

        v = Ctx->Callees[CI];
        OS << "Callees: ";
        for (FuncSet::iterator j = v.begin(), ej = v.end();
             j != ej; ++j) {
            OS << (*j)->getName() << "::";
        }
        OS << "\n";
        if (v.empty()) {
            OS << "!!EMPTY =>" << *CI->getCalledValue()<<"\n";
            OS<< "Uninitialized function pointer is dereferenced!\n";
        }
    }
    RES_REPORT("\n[End of dumpCallees]\n");
}

void CallGraphPass::dumpCallers() {
    RES_REPORT("\n[dumpCallers]\n");
    for (auto M : Ctx->Callers) {
        Function *F = M.first;
        CallInstSet &CIS = M.second;
        RES_REPORT("F : " << F->getName() << "\n");

        for (CallInst *CI : CIS) {
            Function *CallerF = CI->getParent()->getParent();
            RES_REPORT("\t");
            if (CallerF && CallerF->hasName()) {
                RES_REPORT("(" << CallerF->getName() << ") ");
            } else {
                RES_REPORT("(anonymous) ");
            }

            RES_REPORT(*CI << "\n");
        }
    }
    RES_REPORT("\n[End of dumpCallers]\n");
}
