#ifndef STRUCT_ANALYZER_H
#define STRUCT_ANALYZER_H

#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/Support/raw_ostream.h>

#include <vector>
#include <map>
#include <set>

// Every struct type T is mapped to the vectors fieldSize and offsetMap.
// If field [i] in the expanded struct T begins an embedded struct, fieldSize[i] is the # of fields in the largest such struct, else S[i] = 1.
// Also, if a field has index (j) in the original struct, it has index offsetMap[j] in the expanded struct.
class StructInfo
{
private:
	// FIXME: vector<bool> is considered to be BAD C++ practice. We have to switch to something else like deque<bool> some time in the future
	std::vector<bool> arrayFlags;
	std::vector<bool> pointerFlags;
	std::vector<bool> unionFlags;
	std::vector<unsigned> fieldSize;
	std::vector<unsigned> offsetMap;
	std::vector<unsigned> fieldOffset;
	std::vector<unsigned> fieldRealSize;

	// field => type(s) map
	std::map<unsigned, std::set<const llvm::Type*> > elementType;
	
	// the corresponding data layout for this struct
	const llvm::DataLayout* dataLayout;
	void setDataLayout(const llvm::DataLayout* layout) { dataLayout = layout; }

	// real type
	const llvm::StructType* stType;
	void setRealType(const llvm::StructType* st) { stType = st; }

	// defining module
	const llvm::Module* module;
	void setModule(const llvm::Module* M) { module = M; }

	// container type(s)
	std::set<std::pair<const llvm::StructType*, unsigned> > containers;
	void addContainer(const llvm::StructType* st, unsigned offset)
	{
		containers.insert(std::make_pair(st, offset));
	}

	static const llvm::StructType* maxStruct;
	static unsigned maxStructSize;
	uint64_t allocSize;

	bool finalized;

	void addOffsetMap(unsigned newOffsetMap) { offsetMap.push_back(newOffsetMap); }
	void addField(unsigned newFieldSize, bool isArray, bool isPointer, bool isUnion)
	{
		fieldSize.push_back(newFieldSize);
		arrayFlags.push_back(isArray);
		pointerFlags.push_back(isPointer);
		unionFlags.push_back(isUnion);
	}
	void addFieldOffset(unsigned newOffset) { fieldOffset.push_back(newOffset); }
	void addRealSize(unsigned size) { fieldRealSize.push_back(size); }
	void appendFields(const StructInfo& other)
	{
		if (!other.isEmpty()) {
			fieldSize.insert(fieldSize.end(), (other.fieldSize).begin(), (other.fieldSize).end());
		}
		arrayFlags.insert(arrayFlags.end(), (other.arrayFlags).begin(), (other.arrayFlags).end());
		pointerFlags.insert(pointerFlags.end(), (other.pointerFlags).begin(), (other.pointerFlags).end());
		unionFlags.insert(unionFlags.end(), (other.unionFlags).begin(), (other.unionFlags).end());
		fieldRealSize.insert(fieldRealSize.end(), (other.fieldRealSize).begin(), (other.fieldRealSize).end());
	}
	void appendFieldOffset(const StructInfo& other)
	{
		unsigned base = fieldOffset.back();
		for (auto i : other.fieldOffset) {
			if (i == 0) continue;
			fieldOffset.push_back(i + base);
		}
	}
	void addElementType(unsigned field, const llvm::Type* type) { elementType[field].insert(type); }
	void appendElementType(const StructInfo& other)
	{
		unsigned base = fieldSize.size();
		for (auto item : other.elementType)
			elementType[item.first + base].insert(item.second.begin(), item.second.end());
	}

	// Must be called after all fields have been analyzed
	void finalize()
	{
		assert(fieldSize.size() == arrayFlags.size());
		assert(pointerFlags.size() == arrayFlags.size());
		assert(unionFlags.size() == arrayFlags.size());
		unsigned numField = fieldSize.size();
		if (numField == 0)
			fieldSize.resize(1);
		fieldSize[0] = numField;
		if (stType->isSized()) 
			allocSize = dataLayout->getTypeAllocSize(const_cast<llvm::StructType*>(stType));
		else
			allocSize = 0;
		finalized = true;
	}

	static void updateMaxStruct(const llvm::StructType* st, unsigned structSize)
	{
		if (structSize > maxStructSize) {
			maxStruct = st;
			maxStructSize = structSize;
		}
	}
public:
	bool isFinalized() {
		return finalized;
	}

	// # fields == # arrayFlags == # pointer flags
	// size => # of fields????
	// getExpandedSize => # of unrolled fields???

	typedef std::vector<unsigned>::const_iterator const_iterator;
	unsigned getSize() const { return offsetMap.size(); }
	unsigned getExpandedSize() const { return arrayFlags.size(); }

	bool isEmpty() const { return (fieldSize[0] == 0);}
	bool isFieldArray(unsigned field) const { return arrayFlags.at(field); }
	bool isFieldPointer(unsigned field) const { return pointerFlags.at(field); }
	bool isFieldUnion(unsigned field) const { return unionFlags.at(field); }
	unsigned getOffset(unsigned off) const { return offsetMap.at(off); }
	const llvm::Module* getModule() const { return module; }
	const llvm::DataLayout* getDataLayout() const { return dataLayout; }
	const llvm::StructType* getRealType() const { return stType; }
	const uint64_t getAllocSize() const { return allocSize; }
	unsigned getFieldRealSize(unsigned field) const { return fieldRealSize.at(field); }
	unsigned getFieldOffset(unsigned field) const { return fieldOffset.at(field); }
	std::set<const llvm::Type*> getElementType(unsigned field) const
	{
		auto itr = elementType.find(field);
		if (itr != elementType.end())
			return itr->second;
		else
			return std::set<const llvm::Type*>();
	}
	const llvm::StructType* getContainer(const llvm::StructType* st, unsigned offset) const
	{
		assert(!st->isOpaque());
		if (containers.count(std::make_pair(st, offset)) == 1)
			return st;
		else
			return nullptr;
	}

	static unsigned getMaxStructSize() { return maxStructSize; }

	friend class StructAnalyzer;
};

// Construct the necessary StructInfo from LLVM IR
// This pass will make GEP instruction handling easier
class StructAnalyzer
{
private:
	// Map llvm type to corresponding StructInfo
	typedef std::map<const llvm::StructType*, StructInfo> StructInfoMap;
	StructInfoMap structInfoMap;

	// Map struct name to llvm type
	typedef std::map<const std::string, const llvm::StructType*> StructMap;
	StructMap structMap;

	// Expand (or flatten) the specified StructType and produce StructInfo
	StructInfo& addStructInfo(const llvm::StructType* st, const llvm::Module* M, const llvm::DataLayout* layout);
	// If st has been calculated before, return its StructInfo; otherwise, calculate StructInfo for st
	StructInfo& computeStructInfo(const llvm::StructType* st, const llvm::Module *M, const llvm::DataLayout* layout);
	// update container information
	void addContainer(const llvm::StructType* container, StructInfo& containee, unsigned offset, const llvm::Module* M);
public:
	StructAnalyzer() {}

	// Return NULL if info not found
	const StructInfo* getStructInfo(const llvm::StructType* st, llvm::Module* M) const;
	size_t getSize() const { return structMap.size(); }
	bool getContainer(std::string stid, const llvm::Module* M, std::set<std::string> &out) const;
	//bool getContainer(const llvm::StructType* st, std::set<std::string> &out) const;

	void run(llvm::Module* M, const llvm::DataLayout* layout);

	void printStructInfo() const;
};

#endif
