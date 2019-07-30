/*
 * Safe stack analysis
 *
 * Copyright (C) 2014 Volodymyr Kuznetsov
 * Copyright (C) 2015 - 2016 Chengyu Song
 *
 * For licensing details see LICENSE
 */


#define DEBUG_TYPE "safe_stack"
#include <llvm/Support/Debug.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/CallSite.h>
#include <llvm/Pass.h>
#include <llvm/ADT/Triple.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_os_ostream.h>
#include <set>
#include <map>
#include <queue>
#include <fstream>

#include "SafeStack.h"

using namespace llvm;

#define SSS_DEBUG(stmt) KA_LOG(2, stmt)

STATISTIC(NumFunctions, "Total number of functions");
STATISTIC(NumUnsafeStackFunctions, "Number of functions with unsafe stack");

STATISTIC(NumAllocas, "Total number of allocas");
STATISTIC(NumUnsafeStaticAllocas, "Number of unsafe static allocas");
STATISTIC(NumUnsafeDynamicAllocas, "Number of unsafe dynamic allocas");
STATISTIC(NumUnsafeStackStore, "Number of unsafe stack pointer store");
STATISTIC(NumUnsafeStackGEP, "Number of unsafe stack pointer alrithmetic");
STATISTIC(NumUnsafeStackCall, "Number of unsafe stack pointer passed as argument");
STATISTIC(NumUnsafeStackRet, "Number of unsafe stack pointer returned");

/// Check whether a given variable is a stack pointer
bool SafeStackPass::isStackPointer(Value *V) {

	SmallPtrSet<Value*, 16> Visited;
	SmallVector<Value*, 8> WorkList;

	Visited.insert(V);
	WorkList.push_back(V);

	while (!WorkList.empty()) {
		Value *v = WorkList.pop_back_val();

		// handle different source type
		if (Argument *arg = dyn_cast<Argument>(v)) {
			// FIXME: check whether an argument is ALWAYS from stack
			//
			SSS_DEBUG("\tARG: " << *arg);
			if (Function *pf = arg->getParent()) {
				SSS_DEBUG(" <<<<< " << pf->getName());
			}
			SSS_DEBUG("\n");
			return true;
		} else if (InlineAsm *iasm = dyn_cast<InlineAsm>(v)) {
			SSS_DEBUG("\t INLINE_ASM: " << iasm->getAsmString() << "\n");
			return false;
		}

		User *u = dyn_cast<User>(v);
		if (u == NULL) {
			continue;
		}

		if (Constant *cv = dyn_cast<Constant>(u)) {
			SSS_DEBUG("\tCONST: " << *cv << "\n");
			continue;
		}

		Instruction *I = dyn_cast<Instruction>(u);
		if (I == NULL)
			continue;

		if (isa<AllocaInst>(I)) {
			SSS_DEBUG("\t ALLOCA: " << *I <<"\n");
			return true;
		}

		if (isa<CallInst>(I) || isa<InvokeInst>(I))
			// Any buffer returned by a function should
			// never be a stack
			return false;

		if (isa<GetElementPtrInst>(I)) {
			// Check the base pointer only
			//
			Value *base = I->getOperand(0);
			if (Visited.count(base) == 0) {
				Visited.insert(base);
				WorkList.push_back(base);
			}
			continue;
		}

		// follow the def chain
		for (User::op_iterator oi = I->op_begin(), oe = I->op_end();
				 oi != oe; ++oi) {
			if (Visited.count(*oi) == 0) {
				Visited.insert(*oi);
				WorkList.push_back(*oi);
			}
		}
	}
	// by default, conservatively assumes its unsafe
	return false;
}

bool SafeStackPass::isSafeCall(CallInst *CI, unsigned ArgNo, uint64_t Size) {

	// FIXME: assume inline asm as always safe
	if (CI->isInlineAsm())
		return true;

	FuncSet &FS = Ctx->Callees[CI];
	if (FS.empty()) {
		WARNING("Cannot find callee(s), assumes unsafe\n");
		return true;
	}

	bool ret = true;
	for (Function *F : FS) {
		// check arg_size
		if (!F->isVarArg() && CI->getNumArgOperands() != F->arg_size()) {
			WARNING("Arg mismatch: " << F->getName() << "\n");
			continue;
		}

		std::pair<Function*, unsigned> key = std::make_pair(F, ArgNo);
		fi_iterator fi = FuncInfo.find(key);
		if (fi != FuncInfo.end()) {
			ret &= fi->second;
			continue;
		}

		// FIXME: if recursive, assume ??safe??
		fi = FuncInfo.insert(std::make_pair(key, true)).first;

#if LLVM_VERSION_MAJOR <= 4
		if (F->doesNotCapture(ArgNo)) {
			// LLVM 'nocapture' attribute is only set for arguments whose address
			// is not stored, passed around, or used in any other non-trivial way.
			// We assume that passing a pointer to an object as a 'nocapture'
			// argument is safe.
			fi->second = true;
			continue;
		}
#endif

		if (F->isIntrinsic()) {
			// intrinsic, assumes safe
			fi->second = true;
			continue;
		}

		if (F->isVarArg()) {
			// FIXME vararg assmes safe
			fi->second = true;
			continue;
		}

		if (F->isDeclaration()) {
			if (SafeFuncs.count(F->getName().str())) {
				fi->second = true;
				continue;
			}

			// try to find the definition
			StringRef FuncName = F->getName();

			// for unknown reason, llvm tends to use SyS for syscall
			// making sys_* pure declaration, try to solve this here
			//
			if (FuncName.startswith("sys_")) {
				std::string SyS = "SyS_" + FuncName.substr(4).str();
				FuncName = SyS;
			}

			auto itr = Ctx->Funcs.find(FuncName);
			if (itr != Ctx->Funcs.end())
				F = itr->second;

			// try again
			if (F->isDeclaration()) {
				// no body, assumes unsafe
				WARNING("Declaration only: " << F->getName() << "\n");
				fi->second = false;
				return false;
			}
		}

		unsigned index = 0;
		Argument *A = nullptr;
		for (Function::arg_iterator ai = F->arg_begin(), ae = F->arg_end();
				 ai != ae; ++ai, ++index) {
			if (index == ArgNo) {
				A = &*ai;
				break;
			}
		}
		assert(A != nullptr);
		SSS_DEBUG("Check function " << F->getName() << " arg = " << *A << "\n");

		fi->second = isSafeUse(A, Size);
		if (!fi->second)
			SSS_DEBUG("Unsafe function: " << F->getName() << "\n");
		ret &= fi->second;
	}
	return ret;
}

bool SafeStackPass::isSafeGEP(GetElementPtrInst *GEP, uint64_t Size) {
#ifdef DO_RANGE_ANALYSIS
	Type *Ty = GEP->getPointerOperand()->getType();
	Ty = Ty->getContainedType(0);
	SSS_DEBUG("Ty = " << *Ty << "\n");

	// look up index range
	Value *Index = *(GEP->idx_end() - 1);
	BasicBlock *BB = GEP->getParent();

	auto itr1 = Ctx->FuncVRMs.find(BB);
	if (itr1 == Ctx->FuncVRMs.end())
		return false;
	auto itr2 = itr1->second.find(Index);
	if (itr2 == itr1->second.end())
		return false;

	uint64_t index = itr2->second.getUnsignedMax().getZExtValue();
	SSS_DEBUG("CR = " << itr2->second << ", idx = " << index << "\n");

	// check bound
	if (StructType *STy = dyn_cast<StructType>(Ty)) {
		SSS_DEBUG("ST, NELM = " << STy->getNumElements() << "\n");
		return index < STy->getNumElements();
	} else if (ArrayType *ATy = dyn_cast<ArrayType>(Ty)) {
		SSS_DEBUG("Array, NELM = " << ATy->getNumElements() << "\n");
		return index < ATy->getNumElements();
	} else {
		SSS_DEBUG("OTHER, size = " << Size << "\n");
		return index < Size;
	}
#else
	if (GEP->hasAllConstantIndices())
		return true;
	else
		return false;
#endif
}

/// Check whether a given value (V) should be put on the safe
/// stack or not. The function analyzes all uses of AI and checks whether it is
/// only accessed in a memory safe way (as decided statically).
bool SafeStackPass::isSafeUse(Value *V, uint64_t Size) {
	// Go through all uses of this alloca and check whether all accesses to the
	// allocated object are statically known to be memory safe and, hence, the
	// object can be placed on the safe stack.

	SmallPtrSet<CallInst*, 4> CallSites;
	SmallPtrSet<Value*, 16> Visited;
	SmallVector<Value*, 8> WorkList;
	WorkList.push_back(V);

	// A DFS search through all uses of the alloca in bitcasts/PHI/GEPs/etc.
	while (!WorkList.empty()) {
		Value *v = WorkList.pop_back_val();
		for (Use &UI : v->uses()) {
			Instruction *I = dyn_cast<Instruction>(UI.getUser());
			assert(v == UI.get());
			if (I == NULL)
				continue;

			// handle cast and binary ops here, opcode is not clean
			if (isa<CastInst>(I) || isa<BinaryOperator>(I)) {
				if (Visited.insert(I).second)
					WorkList.push_back(I);
				continue;
			}

			switch (I->getOpcode()) {
			case Instruction::Load:
				// Loading from a pointer is safe
				break;
			case Instruction::VAArg:
				// "va-arg" from a pointer is safe
				break;
			case Instruction::Store:
				if (V == I->getOperand(0)) {
					// Stored the pointer - check if the target pointer points to heap
					//
					if (!isStackPointer(I->getOperand(1))) {
						if (isa<AllocaInst>(V))
							NumUnsafeStackStore++;
						SSS_DEBUG("Unsafe store " << *I << "\n");
						return false;
					}
				}
				// Storing to the pointee is safe
				break;

			case Instruction::GetElementPtr:
				// We assume that GEP on static alloca with constant indices is safe,
				// otherwise a compiler would detect it and warn during compilation.

				if (Size == 0) {
					// However, if the array size itself is not constant, the access
					// might still be unsafe at runtime.
					if (isa<AllocaInst>(V))
						NumUnsafeStackGEP++;
					SSS_DEBUG("Unsafe GEP - nc size" << *I << "\n");
					return false;
				}

				if (!cast<GetElementPtrInst>(I)->hasAllConstantIndices() &&
					!isSafeGEP(cast<GetElementPtrInst>(I), Size)) {
					// GEP with non-constant indices can lead to memory errors
					//
					if (isa<AllocaInst>(V))
						NumUnsafeStackGEP++;
					SSS_DEBUG("Unsafe GEP - nc indices" << *I << "\n");
					return false;
				}

				/* fallthough */
			case Instruction::PHI:
			case Instruction::Select:
				// The object can be safe or not, depending on how the result of the
				// BitCast/PHI/Select/GEP/etc. is used.
				if (Visited.insert(I).second)
					WorkList.push_back(cast<Instruction>(I));
				break;

			case Instruction::ICmp:
			case Instruction::Switch:
				break;

			case Instruction::Ret:
				// Value returned is always considered as unsafe
				if (isa<AllocaInst>(V))
					NumUnsafeStackRet++;
				SSS_DEBUG("Unsafe RET" << *I << "\n");
				return false;

			case Instruction::Call: {
				CallInst *CI = cast<CallInst>(I);
				CallSites.insert(CI);
				break;
			}

			default:
				// The object is unsafe if it is used in any other way.
				SSS_DEBUG("UNKNOWN use " << *I << "\n");
				return false;
			}
		}
	}

	// Handle calls at the end to minimize affects of recursion
	bool ret = true;
	for (CallInst *CI : CallSites) {
		// Given we don't care about information leak attacks at this point,
		// the object is considered safe if a pointer to it is passed to a
		// function that only reads memory nor returns any value. This function
		// can neither do unsafe writes itself nor capture the pointer (or
		// return it) to do unsafe writes to it elsewhere. The function also
		// shouldn't unwind (a readonly function can leak bits by throwing an
		// exception or not depending on the input value).
		if (CI->onlyReadsMemory() /* && CS.doesNotThrow()*/ &&
				CI->getType()->isVoidTy())
			continue;

		SSS_DEBUG("Check CallSite " << *CI << "\n");
#if 0
		CallSite CS(CI);
		CallSite::arg_iterator B = CS.arg_begin(), E = CS.arg_end();
		for (CallSite::arg_iterator A = B; A != E; ++A)
			if (A->get() == V && !CS.doesNotCapture(A - B))
			// The parameter is not marked 'nocapture' - unsafe
			return false;
#else
		unsigned i = 0;
		for (Use &A : CI->arg_operands()) {
			if (A.get() == V && !isSafeCall(CI, i, Size)) {
				// The parameter is not marked 'nocapture' - unsafe
				//if (isa<AllocaInst>(V))
				//	NumUnsafeStackCall++;
				SSS_DEBUG("Unsafe call " << *CI << "\n");
				ret = false;
			}
			++i;
		}
#endif
	}

	// All uses of the alloca are safe, we can place it on the safe stack.
	return ret;
}

bool SafeStackPass::runOnFunction(Function *F) {
	++NumFunctions;

	SmallVector<AllocaInst*, 16> StaticAllocas;
	SmallVector<AllocaInst*, 4> DynamicAllocas;
	SmallVector<ReturnInst*, 4> Returns;

	// Collect all points where stack gets unwound and needs to be restored
	// This is only necessary because the runtime (setjmp and unwind code) is
	// not aware of the unsafe stack and won't unwind/restore it prorerly.
	// To work around this problem without changing the runtime, we insert
	// instrumentation to restore the unsafe stack pointer when necessary.
	SmallVector<Instruction*, 4> StackRestorePoints;

	// Find all static and dynamic alloca instructions that must be moved to the
	// unsafe stack, all return instructions and stack restore points
	for (inst_iterator It = inst_begin(F), Ie = inst_end(F); It != Ie; ++It) {
		Instruction *I = &*It;

		if (AllocaInst *AI = dyn_cast<AllocaInst>(I)) {
			++NumAllocas;

			uint64_t size = 0;
			if (AI->isArrayAllocation()) {
				Value *AS = AI->getArraySize();
				if (ConstantInt *INT = dyn_cast<ConstantInt>(AS))
					size = INT->getZExtValue();
			} else {
				Type *Ty = AI->getType();
				Ty = Ty->getContainedType(0);
				if (StructType *STy = dyn_cast<StructType>(Ty)) {
					size = STy->getNumElements();
				} else if (ArrayType *ATy = dyn_cast<ArrayType>(Ty)) {
					size = ATy->getNumElements();
				} else {
					size = 1;
				}
			}

			SSS_DEBUG("Alloca:" << *AI << ", size = " << size << ", F = " << F->getName() << "\n");

			if (isSafeUse(AI, size))
				continue;

			if (AI->isStaticAlloca()) { // buffer with constant size
				++NumUnsafeStaticAllocas;
				StaticAllocas.push_back(AI);
			} else {
				++NumUnsafeDynamicAllocas; // buffer with variable size
				DynamicAllocas.push_back(AI);
			}

			// ugly ...
			Type *AT = AI->getAllocatedType();
			if (AT->isIntegerTy() || AT->isPointerTy())
				continue;

			SSS_DEBUG("UnsafeAlloc:" << F->getParent()->getModuleIdentifier() << ":"
					  << F->getName() << ":"
					  << AI->getName() << ":"
					  << *AI << "\n");

		} else if (ReturnInst *RI = dyn_cast<ReturnInst>(I)) {
			Returns.push_back(RI);

		} else if (CallInst *CI = dyn_cast<CallInst>(I)) {
			// setjmps require stack restore
			if (CI->getCalledFunction() && CI->canReturnTwice())
					//CI->getCalledFunction()->getName() == "_setjmp")
				StackRestorePoints.push_back(CI);

		} else if (LandingPadInst *LP = dyn_cast<LandingPadInst>(I)) {
			// Excpetion landing pads require stack restore
			StackRestorePoints.push_back(LP);
		}
	}

	if (!StaticAllocas.empty() || !DynamicAllocas.empty())
		++NumUnsafeStackFunctions;

	return false;
}

bool SafeStackPass::doInitialization(Module *M) {
	// mark these functions as safe
	std::string SF[] = {
		"set_bit",
		"clear_bit",
		"__copy_from_user",
		"memset",
		"fpsimd_load_state",
		"get_user_pages_fast",
		"probe_kernel_read",
		"save_stack_trace_regs",
		"ce_aes_ccm_auth_data",
	};

	for (auto S : SF) {
		SafeFuncs.insert(S);
	}

	return false;
}

bool SafeStackPass::doFinalization(Module *M) {
	return false;
}

bool SafeStackPass::doModulePass(Module *M) {
	bool changed = true, ret = false;

	while (changed) {
		changed = false;
		for (Function &F : *M)
			changed |= runOnFunction(&F);
		ret |= changed;
	}
	return ret;
}

static void PrintStat(raw_ostream &OS, Statistic &S) {
	OS << format("%8u %s - %s\n", S.getValue(), S.getName(), S.getDesc());
}

void SafeStackPass::dumpStats() {
	outs() << "SafeStack Statistics:\n";

	PrintStat(outs(), NumFunctions);
	PrintStat(outs(), NumUnsafeStackFunctions);
	//PrintStat(outs(), NumUnsafeStackRestorePointsFunctions);

	PrintStat(outs(), NumAllocas);
	PrintStat(outs(), NumUnsafeStaticAllocas);
	PrintStat(outs(), NumUnsafeDynamicAllocas);
	//PrintStat(outs(), NumUnsafeStackRestorePoints);

	PrintStat(outs(), NumUnsafeStackStore);
	PrintStat(outs(), NumUnsafeStackGEP);
	PrintStat(outs(), NumUnsafeStackCall);
}

