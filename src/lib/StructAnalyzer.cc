/*
 * Data structure
 *
 * Copyright (C) 2015 Jia Chen
 * Copyright (C) 2015 - 2018 Chengyu Song
 *
 * For licensing details see LICENSE
 */

#include <llvm/IR/TypeFinder.h>
#include <llvm/Support/raw_ostream.h>

#include "StructAnalyzer.h"
#include "Annotation.h"

using namespace llvm;

// Initialize max struct info
const StructType* StructInfo::maxStruct = NULL;
unsigned StructInfo::maxStructSize = 0;

void StructAnalyzer::addContainer(const StructType* container, StructInfo& containee, unsigned offset, const Module* M)
{
	containee.addContainer(container, offset);
	// recursively add to all nested structs
	const StructType* ct = containee.stType;
	for (StructType::element_iterator itr = ct->element_begin(), ite = ct->element_end(); itr != ite; ++itr) {
		Type* subType = *itr;
		// strip away array
		while (const ArrayType* arrayType = dyn_cast<ArrayType>(subType))
			subType = arrayType->getElementType();
		if (const StructType* structType = dyn_cast<StructType>(subType)) {
			if (!structType->isLiteral()) {
				auto real = structMap.find(getScopeName(structType, M));
				if (real != structMap.end())
					structType = real->second;
			}
			auto itr = structInfoMap.find(structType);
			assert(itr != structInfoMap.end());
			StructInfo& subInfo = itr->second;
			for (auto item : subInfo.containers) {
				if (item.first == ct)
					addContainer(container, subInfo, item.second + offset, M);
			}
		}
	}
}

StructInfo& StructAnalyzer::computeStructInfo(const StructType* st, const Module* M, const DataLayout* layout)
{
	if (!st->isLiteral()) {
		auto real = structMap.find(getScopeName(st, M));
		if (real != structMap.end())
			st = real->second;
	}

	auto itr = structInfoMap.find(st);
	if (itr != structInfoMap.end())
		return itr->second;
	else
		return addStructInfo(st, M, layout);
}

StructInfo& StructAnalyzer::addStructInfo(const StructType* st, const Module* M, const DataLayout* layout)
{
	unsigned numField = 0;
	unsigned fieldIndex = 0;
	unsigned currentOffset = 0;
	StructInfo& stInfo = structInfoMap[st];

	if (stInfo.isFinalized())
		return stInfo;

	const StructLayout* stLayout = layout->getStructLayout(const_cast<StructType*>(st));
	stInfo.addElementType(0, const_cast<StructType*>(st));
	for (StructType::element_iterator itr = st->element_begin(), ite = st->element_end(); itr != ite; ++itr) {
		const Type* subType = *itr;
		currentOffset = stLayout->getElementOffset(fieldIndex++);
		stInfo.addFieldOffset(currentOffset);

		bool isArray = isa<ArrayType>(subType);
		// Treat an array field as a single element of its type
		while (const ArrayType* arrayType = dyn_cast<ArrayType>(subType))
			subType = arrayType->getElementType();

		// record type after stripping array
		stInfo.addElementType(numField, subType);

		// The offset is where this element will be placed in the expanded struct
		stInfo.addOffsetMap(numField);

		// Nested struct
		if (const StructType* structType = dyn_cast<StructType>(subType)) {
			assert(!structType->isOpaque() && "Nested opaque struct");
			StructInfo& subInfo = computeStructInfo(structType, M, layout);
			assert(subInfo.isFinalized());

			addContainer(st, subInfo, currentOffset, M);

			// Copy information from this substruct
			stInfo.appendFields(subInfo);
			stInfo.appendFieldOffset(subInfo);
			stInfo.appendElementType(subInfo);

			numField += subInfo.getExpandedSize();
		} else {
			stInfo.addField(1, isArray, subType->isPointerTy());
			++numField;
		}
	}

	stInfo.setRealType(st);
	stInfo.setDataLayout(layout);
	stInfo.finalize();
	StructInfo::updateMaxStruct(st, numField);

	return stInfo;
}

// We adopt the approach proposed by Pearce et al. in the paper "efficient field-sensitive pointer analysis of C"
void StructAnalyzer::run(Module* M, const DataLayout* layout)
{
	TypeFinder usedStructTypes;
	usedStructTypes.run(*M, false);
	for (TypeFinder::iterator itr = usedStructTypes.begin(), ite = usedStructTypes.end(); itr != ite; ++itr) {
		const StructType* st = *itr;

		// handle non-literal first
		if (st->isLiteral()) {
			addStructInfo(st, M, layout);
			continue;
		}

		// only add non-opaque type
		if (!st->isOpaque()) {
			// process new struct only
			if (structMap.insert(std::make_pair(getScopeName(st, M), st)).second)
				addStructInfo(st, M, layout);
		}
	}
}

const StructInfo* StructAnalyzer::getStructInfo(const StructType* st, Module* M) const
{
	// try struct pointer first, then name
	auto itr = structInfoMap.find(st);
	if (itr != structInfoMap.end())
		return &(itr->second);

	if (!st->isLiteral()) {
		auto real = structMap.find(getScopeName(st, M));
		//assert(real != structMap.end() && "Cannot resolve opaque struct");
		if (real != structMap.end())
			st = real->second;
	}

	itr = structInfoMap.find(st);
	if (itr == structInfoMap.end())
		return nullptr;
	else
		return &(itr->second);
}

bool StructAnalyzer::getContainer(std::string stid, const Module* M, std::set<std::string> &out) const
{
	bool ret = false;

	auto real = structMap.find(stid);
	if (real == structMap.end())
		return ret;

	const StructType* st = real->second;
	auto itr = structInfoMap.find(st);
	assert(itr != structInfoMap.end() && "Cannot find target struct info");
	for (auto container_pair : itr->second.containers) {
		const StructType* container = container_pair.first;
		if (container->isLiteral())
			continue;
		std::string id = container->getStructName().str();
		if (id.find("struct.anon") == 0 ||
			id.find("union.anon") == 0) {
			// anon struct, get its parent instead
			id = getScopeName(container, M);
			ret |= getContainer(id, M, out);
		} else {
			out.insert(id);
		}
		ret = true;
	}

	return ret;
}

//bool StructAnalyzer::getContainer(const StructType* st, std::set<std::string> &out) const
//{
//}

void StructAnalyzer::printStructInfo() const
{
	errs() << "----------Print StructInfo------------\n";
	for (auto const& mapping: structInfoMap) {
		errs() << "Struct " << mapping.first << ": sz < ";
		const StructInfo& info = mapping.second;
		for (auto sz: info.fieldSize)
			errs() << sz << " ";
		errs() << ">, offset < ";
		for (auto off: info.offsetMap)
			errs() << off << " ";
		errs() << ">\n";
	}
	errs() << "----------End of print------------\n";
}
