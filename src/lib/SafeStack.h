#ifndef _SAFE_STACK_H
#define _SAFE_STACK_H

#include "Global.h"

class SafeStackPass : public IterativeModulePass {
	std::map<std::pair<Function*, unsigned>, bool> FuncInfo;
	typedef std::map<std::pair<Function*, unsigned>, bool>::iterator fi_iterator;

	std::set<std::string> SafeFuncs;

	bool runOnFunction(llvm::Function*);
	bool isSafeUse(llvm::Value*, uint64_t);
	bool isStackPointer(llvm::Value*);
	bool isSafeCall(llvm::CallInst*, unsigned, uint64_t);
	bool isSafeGEP(llvm::GetElementPtrInst*, uint64_t);

public:
	SafeStackPass(GlobalContext *Ctx_)
		: IterativeModulePass(Ctx_, "SafeStackStats") { }

	virtual bool doModulePass(llvm::Module*);
	virtual bool doInitialization(llvm::Module*);
	virtual bool doFinalization(llvm::Module*);

	void dumpStats();
	void dumpUnsafeAlloc();
};
#endif
