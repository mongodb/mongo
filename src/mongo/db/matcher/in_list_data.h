/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/lazily_initialized.h"

#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional.hpp>

namespace mongo {
class InListElemLessThan {
public:
    using TypeTags = sbe::value::TypeTags;
    using Value = sbe::value::Value;
    using TagValuePair = std::pair<TypeTags, Value>;
    using Cmp = StringDataComparator;

    static constexpr BSONObj::ComparisonRulesSet kIgnoreFieldName = 0;

    explicit InListElemLessThan(const Cmp* cmp) : _cmp(cmp) {}

    inline bool operator()(const BSONElement& lhs, const BSONElement& rhs) const {
        return compareImpl(lhs, rhs);
    }

    // Overloads of operator() that compare a BSONElement with a TagValuePair.
    inline bool operator()(const BSONElement& lhs, TagValuePair rhs) const {
        return compareImpl(sbe::bson::convertFrom<true>(lhs), rhs);
    }
    inline bool operator()(TagValuePair lhs, const BSONElement& rhs) const {
        return compareImpl(lhs, sbe::bson::convertFrom<true>(rhs));
    }

    // Overloads of operator() that compare a BSONElement's type with a BSONType.
    inline bool operator()(const BSONElement& lhs, BSONType rhsType) const {
        return canonicalizeBSONType(lhs.type()) < canonicalizeBSONType(rhsType);
    }
    inline bool operator()(BSONType lhsType, const BSONElement& rhs) const {
        return canonicalizeBSONType(lhsType) < canonicalizeBSONType(rhs.type());
    }

private:
    inline bool compareImpl(const BSONElement& lhs, const BSONElement& rhs) const {
        return lhs.woCompare(rhs, kIgnoreFieldName, _cmp) < 0;
    }

    inline bool compareImpl(TagValuePair lhs, TagValuePair rhs) const {
        auto [lhsTag, lhsVal] = lhs;
        auto [rhsTag, rhsVal] = rhs;
        auto [resTag, resVal] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal, _cmp);
        return resTag == TypeTags::NumberInt32 ? (sbe::value::bitcastTo<int32_t>(resVal) < 0)
                                               : false;
    }

    const Cmp* _cmp = nullptr;
};

class InListElemEqualTo {
public:
    using Cmp = StringDataComparator;

    static constexpr BSONObj::ComparisonRulesSet kIgnoreFieldName = 0;

    explicit InListElemEqualTo(const Cmp* cmp) : _cmp(cmp) {}

    inline bool operator()(const BSONElement& lhs, const BSONElement& rhs) const {
        return lhs.woCompare(rhs, kIgnoreFieldName, _cmp) == 0;
    }

private:
    const Cmp* _cmp = nullptr;
};

/**
 * This class is used by InMatchExpression to represent the contents of the in-list (excluding
 * regex values).
 *
 * InListData supports two concurrency models:
 * - Exclusive model: For a given InListsData object, one thread has non-const access to the object,
 *   and NO other threads have const access or non-const access.
 * - Shared model: For a given InListsData object, any number of threads have const-only access to
 *   the object and NO threads have non-const access.
 *
 * Non-const InListData methods when invoked can assume the current model in use is the "Exclusive"
 * model. Thus they can assume that there aren't other threads potentially reading or writing to the
 * InListData, and therefore it is safe for non-const methods to mutate the InListData.
 *
 * Const InListData methods (because they have to work with both models) must assume that there
 * might be other threads accessing the InListData object via const reference. Const methods
 * therefore cannot mutate the InListData object (with the exception of the '_shared' and
 * '_sortedElements' fields, for which we have appropriate synchronization in place to allow
 * for mutation). Furthermore, const methods cannot return non-const references or pointers to
 * the InListData object or any of its contents.
 *
 * (Note: The rules above regarding concurrency models and const / non-const methods are also
 * applicable to the InMatchExpression class and all the other subclasses of MatchExpression.)
 */
class InListData {
public:
    static constexpr size_t kLargeStringThreshold = 1000u;
    static constexpr BSONObj::ComparisonRulesSet kIgnoreFieldName = 0;

    InListData() : _sortedElements(boost::in_place_init) {}

    InListData(const InListData& other) = delete;
    InListData(InListData&& other) = delete;

    InListData& operator=(const InListData& other) = delete;
    InListData& operator=(InListData&& other) = delete;

    std::shared_ptr<InListData> clone() const {
        return std::shared_ptr<InListData>(new InListData(CloneCtorTag{}, *this));
    }

    bool hasNull() const {
        return _typeMask & getBSONTypeMask(BSONType::null);
    }
    bool hasArray() const {
        return _typeMask & getBSONTypeMask(BSONType::array);
    }
    bool hasObject() const {
        return _typeMask & getBSONTypeMask(BSONType::object);
    }
    bool hasEmptyArray() const {
        return _hasEmptyArray;
    }
    bool hasEmptyObject() const {
        return _hasEmptyObject;
    }
    bool hasNonEmptyArray() const {
        return _hasNonEmptyArray;
    }
    bool hasNonEmptyObject() const {
        return _hasNonEmptyObject;
    }

    uint32_t getTypeMask() const {
        return _typeMask;
    }
    uint64_t getSbeTagMask() const {
        return _sbeTagMask;
    }
    uint64_t getHashSetSbeTagMask() const {
        return _hashSetSbeTagMask;
    }
    bool hasLargeStrings() const {
        return _hasLargeStrings;
    }

    const std::vector<BSONElement>& getElements(bool getSortedAndDeduped = true) const {
        return getSortedAndDeduped ? getSortedElements() : _originalElements;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    // Returns true if the specified BSONElement is equal to one of this InListData's elements,
    // otherwise returns false.
    bool contains(const BSONElement& e) const {
        // If 'e.type()' is not present in _typeMask, bail out and return false.
        if ((getBSONTypeMask(e.type()) & _typeMask) == 0) {
            return false;
        }

        // Use binary search.
        auto elemLt = InListElemLessThan(_collator);
        const auto& elems = getSortedElements();
        return std::binary_search(elems.begin(), elems.end(), e, elemLt);
    }

    bool elementsIsEmpty() const {
        return _originalElements.empty();
    }

    bool hasSingleElement() const {
        return _hasSingleUniqueElement;
    }

    void appendElements(BSONArrayBuilder& bab, bool getSortedAndDeduped = true);

    /**
     * Reduces the potentially large vector of elements to just the first of each "canonical" type.
     * Different types of numbers are not considered distinct.
     *
     * For example, collapses [2, 4, NumberInt(3), "foo", "bar"] into just [2, "foo"].
     */
    std::vector<BSONElement> getFirstOfEachType(bool getSortedAndDeduped = true) const;

    /**
     * This method writes this InListData's elements to the specified stream. If the sorted elements
     * are available then the sorted elements will be used, otherwise '_originalElements' will be
     * This method writes this InListData's elements to the specified stream. If the sorted elements
     * are available then the sorted elements will be used, otherwise '_originalElements' will be
     * used.
     */
    template <typename StreamT>
    void writeToStream(StreamT& stream) const {
        const auto* sortedElems = getSortedElementsIfAvailable();
        const auto& elems = sortedElems ? *sortedElems : _originalElements;

        bool first = true;
        for (const auto& elem : elems) {
            if (first) {
                first = false;
            } else {
                stream << " ";
            }

            stream << elem.toString(false);
        }
    }

    Status setElementsArray(BSONObj arr,
                            bool errorOnRegex = true,
                            const std::function<Status(const BSONElement&)>& fn = {}) {
        const boost::optional<uint32_t> elemsSizeHint = boost::none;
        return setElementsImpl(std::move(arr), {}, errorOnRegex, elemsSizeHint, fn);
    }

    Status setElements(std::vector<BSONElement> elements) {
        return setElementsImpl({}, std::move(elements));
    }

    void setCollator(const CollatorInterface* coll);

    bool isBSONOwned() const {
        return _arr.has_value() && _arr->isOwned();
    }

    const BSONObj& getOwnedBSONStorage() const {
        tassert(8558800, "Expected BSON storage to be owned", isBSONOwned());
        return *_arr;
    }

    void makeBSONOwned();

    MONGO_COMPILER_ALWAYS_INLINE
    bool isShared() const {
        return _shared.load();
    }

    MONGO_COMPILER_ALWAYS_INLINE
    void setShared() const {
        if (!isShared()) {
            _shared.store(true);
        }
    }

    bool elementsHaveBeenSorted() const {
        return getSortedElementsIfAvailable() != nullptr;
    }

private:
    static constexpr auto kCanonicalTypeMinValue = BSONType::minKey;
    static constexpr auto kCanonicalTypeMaxValue = BSONType::maxKey;
    static constexpr std::size_t kCanonicalTypeCardinality =
        stdx::to_underlying(kCanonicalTypeMaxValue) - stdx::to_underlying(kCanonicalTypeMinValue) +
        1;
    struct CloneCtorTag {};
    using SmallBSONElementVector = absl::InlinedVector<BSONElement, 1>;
    using CanonicalTypeMask = std::bitset<kCanonicalTypeCardinality>;

    InListData(CloneCtorTag, const InListData& other);

    MONGO_COMPILER_ALWAYS_INLINE
    static std::unique_ptr<std::vector<BSONElement>> cloneSortedElements(
        const boost::optional<LazilyInitialized<std::vector<BSONElement>>>& sortedElems) {
        // If 'sortedElems' is initialized return a copy of its contents, otherwise return null.
        if (const std::vector<BSONElement>* vec = sortedElems->getIfInitialized()) {
            return std::make_unique<std::vector<BSONElement>>(*vec);
        }
        return {};
    }

    Status setElementsImpl(boost::optional<BSONObj> arr,
                           boost::optional<std::vector<BSONElement>> elementsIn,
                           bool errorOnRegex = true,
                           boost::optional<uint32_t> elemsSizeHint = boost::none,
                           const std::function<Status(const BSONElement&)>& fn = {});

    MONGO_COMPILER_ALWAYS_INLINE
    const std::vector<BSONElement>* getSortedElementsIfAvailable() const {
        // If '_originalElements' is already in sorted order and doesn't have any duplicates,
        // then return a pointer to '_originalElements'.
        if (_wasPreSortedAndDeduped) {
            return &_originalElements;
        }

        return _sortedElements->getIfInitialized();
    }

    MONGO_COMPILER_ALWAYS_INLINE
    const std::vector<BSONElement>& getSortedElements() const {
        // If '_originalElements' is already in sorted order and doesn't have any duplicates,
        // then return a reference to '_originalElements'.
        if (_wasPreSortedAndDeduped) {
            return _originalElements;
        }

        // Get '_sortedElements' (initializing it if needed) and return a reference to it.
        return _sortedElements->get([&] {
            // Copy '_originalElements' into 'elems'.
            auto elems = std::make_unique<std::vector<BSONElement>>(_originalElements);

            // If 'elems' isn't already in sorted order, sort it now.
            if (!_wasPreSorted) {
                auto elemLt = InListElemLessThan(_collator);
                std::sort(elems->begin(), elems->end(), elemLt);
            }

            // De-duplicate 'elems' and return it.
            auto elemEq = InListElemEqualTo(_collator);
            auto newEnd = std::unique(elems->begin(), elems->end(), elemEq);
            if (newEnd != elems->end()) {
                elems->erase(newEnd, elems->end());
            }

            return elems;
        });
    }

    void updateSbeTagMasks();

    void sortAndDedupElementsImpl();

    /**
     * Returns a normalized canonical type of BSON type 'type'. The max returned value is expected
     * to be less than 256.
     */
    static std::size_t getNormalizedCanonicalType(BSONType type);

    // If '_arr' is defined and '_arr->isOwned()' is false, this helper function will call
    // makeOwned() on '_arr' and then it will fixup the BSONElements in '_originalElements'
    // and '_sortedElements'. Otherwise, this helper does nothing and returns.
    void makeArrOwned();

    // Collator used to construct the comparator for comparing elements.
    const CollatorInterface* _collator = nullptr;

    // Bitset that indicates which SBE TypeTags could potentially be equal to an element in
    // '_originalElements'.
    uint64_t _sbeTagMask = 0;
    uint64_t _hashSetSbeTagMask = 0;

    // Bitset that indicates which BSONTypes could potentially be equal to an element in
    // '_originalElements'.
    uint32_t _typeMask = 0;

    // Whether or not '_originalElements' has been initialized.
    bool _elementsInitialized = false;

    // Whether or not '_originalElements' contains an empty array.
    bool _hasEmptyArray = false;

    // Whether or not '_originalElements' contains an empty object.
    bool _hasEmptyObject = false;

    // Whether or not '_originalElements' contains a non-empty array.
    bool _hasNonEmptyArray = false;

    // Whether or not '_originalElements' contains a non-empty object.
    bool _hasNonEmptyObject = false;

    // Whether or not '_originalElements' contains one or more strings whose lengths (in bytes)
    // exceed 'kLargeStringThreshold'.
    bool _hasLargeStrings = false;

    // Whether or not '_originalElements' was pre-sorted.
    bool _wasPreSorted = false;

    // Whether or not '_originalElements' was pre-sorted and pre-deduped.
    bool _wasPreSortedAndDeduped = false;

    // Whether or not the contents of '_originalElements', after de-duping, will consist of exactly
    // one element.
    bool _hasSingleUniqueElement = false;

    // Whether or not this InListData has been "shared". Once an InListData transitions to the
    // "shared" state, it cannot be modified and will remain in the "shared" state for the res
    // of its lifetime.
    mutable AtomicWord<bool> _shared{false};

    // An optional BSON array field. If this field is not boost::none, it will point to a BSON array
    // that contains all of the BSONElements from '_originalElements'.
    boost::optional<BSONObj> _arr;

    // An optional BSON array field. If the 'setElements(BSONObj)' method is called and then later
    // the 'setElements(vector<BSONElement>)' method is called, this field is used to keep the old
    // BSON array alive in case some of the elements in the 'vector<BSONElement>' point to it.
    boost::optional<BSONObj> _oldBackingArr;

    // If '_elementsInitialized' is true, then this field will contain the all elements (with regex
    // type values filtered out). There are the following states of operation: (1) if '_arr' is set,
    // the elements refer to elements of BSON array '_arr'; (2) otherwise, the elements either refer
    // to elements of BSON array '_oldBackingArr' (which is owned), or elements of external to the
    // class unowned BSON arrays.
    std::vector<BSONElement> _originalElements;

    // A lazily initialized vector of BSONElements. When '_sortedElements' has been initialized,
    // it will contain a sorted and deduped copy of the elements from '_originalElements'.
    boost::optional<LazilyInitialized<std::vector<BSONElement>>> _sortedElements;

    // A vector of BSONElements of the first observed elements of each distinct canonical type in
    // '_originalElements'.
    SmallBSONElementVector _firstOfEachTypeElements;
};
}  // namespace mongo
