#ifndef POINTTO_H
#define POINTTO_H

#include <map>

#include "Global.h"
#include "PtsSet.h"
#include "NodeFactory.h"

typedef std::map<NodeIndex, AndersPtsSet> PtsGraph;
typedef std::map<const llvm::Instruction*, PtsGraph> NodeToPtsGraph;

void populateNodeFactory(GlobalContext &GlobalCtx);

int64_t getGEPOffset(const llvm::Value* value, const llvm::DataLayout* dataLayout);
unsigned offsetToFieldNum(const llvm::Value* ptr, int64_t offset, const llvm::DataLayout* dataLayout,
    const StructAnalyzer *structAnalyzer, llvm::Module* module);
int64_t getGEPInstFieldNum(const llvm::GetElementPtrInst* gepInst,
    const llvm::DataLayout* dataLayout, const StructAnalyzer& structAnalyzer, llvm::Module* module);

#endif
