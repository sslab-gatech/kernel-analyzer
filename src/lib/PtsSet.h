#ifndef ANDERSEN_PTSSET_H
#define ANDERSEN_PTSSET_H

#include <llvm/ADT/SparseBitVector.h>

// We move the points-to set representation here into a separate class
// The intention is to let us try out different internal implementation of this data-structure (e.g. vectors/bitvecs/sets, ref-counted/non-refcounted) easily
class AndersPtsSet {
private:
    llvm::SparseBitVector<> bitvec;

public:
    using iterator = llvm::SparseBitVector<>::iterator;

    bool has(unsigned idx) {
        return bitvec.test(idx);
    }

    bool insert(unsigned idx) {
        return bitvec.test_and_set(idx);
    }

    bool insert(iterator b, iterator e) {
        bool ret = false;
        for (iterator i = b; i != e; ++i)
            ret |= bitvec.test_and_set(*i);
        return ret;
    }

    void reset(unsigned idx) {
        bitvec.reset(idx);
    }

    // Return true if *this is a superset of other
    bool contains(const AndersPtsSet& other) const {
        return bitvec.contains(other.bitvec);
    }

    // intersectWith: return true if *this and other share points-to elements
    bool intersectWith(const AndersPtsSet& other) const {
        return bitvec.intersects(other.bitvec);
    }

    // Return true if the ptsset changes
    bool unionWith(AndersPtsSet& other) {
        return bitvec |= other.bitvec;
    }

    void clear() {
        bitvec.clear();
    }

    unsigned getSize() const {
        return bitvec.count();  // NOT a constant time operation!
    }

    bool isEmpty() const {
        return bitvec.empty();  // Always prefer using this function to perform empty test
    }

    bool operator!=(const AndersPtsSet& RHS) const {
        return bitvec != RHS.bitvec;
    }

    bool operator==(const AndersPtsSet& RHS) const {
        return bitvec == RHS.bitvec;
    }

    iterator begin() const { return bitvec.begin(); }
    iterator end() const { return bitvec.end(); }
    llvm::iterator_range<iterator> elements() const {
        return llvm::iterator_range<iterator>(begin(), end());
    }
};

#endif
