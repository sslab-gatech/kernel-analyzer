#ifndef _CALL_GRAPH_H
#define _CALL_GRAPH_H

#include "Global.h"

class CallGraphPass : public IterativeModulePass {
private:
    bool runOnFunction(llvm::Function*);
    void processInitializers(llvm::Module*, llvm::Constant*, llvm::GlobalValue*, llvm::StringRef);
    bool findCallees(llvm::CallInst*, FuncSet&);
    bool isCompatibleType(llvm::Type *T1, llvm::Type *T2);
    void findCalleesByType(llvm::CallInst*, FuncSet&);

public:
    CallGraphPass(GlobalContext *Ctx_)
        : IterativeModulePass(Ctx_, "CallGraph") { }
    virtual bool doInitialization(llvm::Module *);
    virtual bool doFinalization(llvm::Module *);
    virtual bool doModulePass(llvm::Module *);

    // debug
    void dumpFuncPtrs();
    void dumpCallees();
    void dumpCallers();
};

#endif
