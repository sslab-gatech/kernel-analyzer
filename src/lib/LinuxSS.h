#ifndef _LINUX_SS_H
#define _LINUX_SS_H

#include <llvm/IR/Dominators.h>
#include <llvm/ADT/SmallSet.h>
#include "Global.h"

#include <set>
#include <unordered_set>

class LinuxSS : public IterativeModulePass {
public:
    typedef std::pair<llvm::Value*, llvm::BasicBlock*> RetPair;
    typedef std::set<RetPair> RetSet;

    typedef llvm::SmallPtrSet<llvm::Value*, 8> ValueSet;
    typedef llvm::SmallPtrSet<llvm::BasicBlock*, 8> BBSet;

#if LLVM_VERSION_MAJOR <= 4
    typedef llvm::DominatorTreeBase<BasicBlock> DomTree;
    typedef llvm::DominatorTreeBase<BasicBlock> PostDomTree;
#else
    typedef llvm::DominatorTreeBase<BasicBlock, false> DomTree;
    typedef llvm::DominatorTreeBase<BasicBlock, true> PostDomTree;
#endif

private:
    PostDomTree *gPDT;
    DomTree *gDT;

    bool runOnFunction(llvm::Function*);
    void collectRetVal(llvm::Value*, llvm::BasicBlock*, RetSet&, ValueSet&);
    bool checkControlDep(BBSet&, BBSet&);
    bool isTrueFalseFunc(llvm::Function*);
    bool isErrorBranch(llvm::BasicBlock*, llvm::BasicBlock*);
    bool dumpControlDep(llvm::BasicBlock*, BBSet&);
    bool collectCondition(llvm::Value*);

public:
    LinuxSS(GlobalContext *Ctx_)
        : IterativeModulePass(Ctx_, "LinuxSS") {
#if LLVM_VERSION_MAJOR <= 4
        gPDT = new PostDomTree(true);
        gDT = new DomTree(false);
#else
        gPDT = new PostDomTree();
        gDT = new DomTree();
#endif
        Ctx->add("SecConds", new std::set<llvm::Value*>());
    }

    ~LinuxSS() {
        delete gPDT;
        delete gDT;
    }

    virtual bool doModulePass(llvm::Module*);
    virtual bool doInitialization(llvm::Module*);
    virtual bool doFinalization(llvm::Module*);
};

#endif
