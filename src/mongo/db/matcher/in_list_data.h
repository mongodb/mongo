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

#include <boost/optional.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {
class InListData {
public:
    static constexpr size_t kLargeStringThreshold = 1000u;
    static constexpr BSONObj::ComparisonRulesSet kIgnoreFieldName = 0;

    class InListElemLessThan {
    public:
        using TypeTags = sbe::value::TypeTags;
        using Value = sbe::value::Value;
        using TagValuePair = std::pair<TypeTags, Value>;
        using Cmp = StringDataComparator;

        explicit InListElemLessThan(const Cmp* cmp) : _cmp(cmp) {}

        inline bool operator()(const BSONElement& lhs, const BSONElement& rhs) const {
            return lhs.woCompare(rhs, kIgnoreFieldName, _cmp) < 0;
        }
        inline bool operator()(const BSONElement& lhs, TagValuePair rhs) const {
            return compareHelper(sbe::bson::convertFrom<true>(lhs), rhs);
        }
        inline bool operator()(TagValuePair lhs, const BSONElement& rhs) const {
            return compareHelper(lhs, sbe::bson::convertFrom<true>(rhs));
        }
        inline bool operator()(const BSONElement& lhs, BSONType rhsType) const {
            return canonicalizeBSONType(lhs.type()) < canonicalizeBSONType(rhsType);
        }
        inline bool operator()(BSONType lhsType, const BSONElement& rhs) const {
            return canonicalizeBSONType(lhsType) < canonicalizeBSONType(rhs.type());
        }

    private:
        inline bool compareHelper(const BSONElement& lhs, const BSONElement& rhs) const {
            return lhs.woCompare(rhs, kIgnoreFieldName, _cmp) < 0;
        }

        inline bool compareHelper(TagValuePair lhs, TagValuePair rhs) const {
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

        explicit InListElemEqualTo(const Cmp* cmp) : _cmp(cmp) {}

        inline bool operator()(const BSONElement& lhs, const BSONElement& rhs) const {
            return lhs.woCompare(rhs, kIgnoreFieldName, _cmp) == 0;
        }

    private:
        const Cmp* _cmp = nullptr;
    };

    InListData() : _hashSet(0, sbe::value::ValueHash{}, sbe::value::ValueEq{}) {}

    InListData(InListData&& other) = delete;

    InListData& operator=(const InListData& other) = delete;
    InListData& operator=(InListData&& other) = delete;

    std::shared_ptr<InListData> clone() const {
        return std::shared_ptr<InListData>(new InListData(*this));
    }

    bool hasNull() const {
        return _typeMask & getBSONTypeMask(BSONType::jstNULL);
    }
    bool hasArray() const {
        return _typeMask & getBSONTypeMask(BSONType::Array);
    }
    bool hasObject() const {
        return _typeMask & getBSONTypeMask(BSONType::Object);
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

    const std::vector<BSONElement>& getElements() {
        return _elements;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    bool contains(const BSONElement& e) {
        // If 'e.type()' is not present in _typeMask, bail out and return false.
        if (!(getBSONTypeMask(e.type()) & _typeMask)) {
            return false;
        }

        // Use binary search.
        auto elemLt = InListElemLessThan(_collator);
        return std::binary_search(_elements.begin(), _elements.end(), e, elemLt);
    }

    bool contains(sbe::value::TypeTags tag, sbe::value::Value val) const {
        constexpr uint64_t stringOrSymbolSbeTagMask =
            (1ull << static_cast<uint64_t>(sbe::value::TypeTags::StringSmall)) |
            (1ull << static_cast<uint64_t>(sbe::value::TypeTags::StringBig)) |
            (1ull << static_cast<uint64_t>(sbe::value::TypeTags::bsonString)) |
            (1ull << static_cast<uint64_t>(sbe::value::TypeTags::bsonSymbol));

        dassert(isPrepared());

        uint64_t mask = 1ull << static_cast<uint64_t>(tag);

        bool searchHashSet = false;

        if ((mask & stringOrSymbolSbeTagMask) == 0u) {
            if (sbe::value::isShallowType(tag) || tag == sbe::value::TypeTags::NumberDecimal) {
                searchHashSet = true;
            } else if ((mask & _sbeTagMask) == 0u) {
                // If 'mask' is not present in '_sbeTagMask', then we know 'tag'/'val' cannot
                // possibly in the in-list so we return false.
                return false;
            }
        } else {
            if (mask & _hashSetSbeTagMask) {
                if (sbe::value::getStringOrSymbolLength(tag, val) <= kLargeStringThreshold) {
                    searchHashSet = true;
                }
            } else if ((mask & _sbeTagMask) == 0u) {
                // If 'mask' is not present in '_sbeTagMask', then we know 'tag'/'val' cannot
                // possibly in the in-list so we return false.
                return false;
            }
        }

        if (searchHashSet) {
            // Search the hash set.
            return _hashSet.find({tag, val}) != _hashSet.end();
        }

        // Use binary search.
        auto beginIt = _elements.begin() + _binarySearchStartOffset;
        auto elemLt = InListElemLessThan(_collator);
        return std::binary_search(beginIt, _elements.end(), std::pair(tag, val), elemLt);
    }

    bool elementsIsEmpty() const {
        return _elements.empty();
    }

    bool hasSingleElement() const {
        return !_hasMultipleUniqueElements && !_elements.empty();
    }

    void appendElements(BSONArrayBuilder& bab, bool getSortedAndDeduped = true);

    void appendOriginalElements(BSONArrayBuilder& bab) const;

    /**
     * Reduces the potentially large vector of elements to just the first of each "canonical" type.
     * Different types of numbers are not considered distinct.
     *
     * For example, collapses [2, 4, NumberInt(3), "foo", "bar", 3, 5] into just [2, "foo"].
     */
    std::vector<BSONElement> getFirstOfEachType(bool getSortedAndDeduped);

    /**
     * This method writes the contents of '_elements' to the specified stream. Note that if
     * '_elements' has not been sorted or deduped yet, this method will leave '_elements' as-is.
     */
    template <typename StreamT>
    void writeToStream(StreamT& stream) {
        for (auto&& e : _elements) {
            stream << e.toString(false) << " ";
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

    bool isSortedAndDeduped() const {
        return _sortedAndDeduped;
    }

    bool isBSONOwned() const {
        return _arr.has_value() && _arr->isOwned();
    }

    bool isPrepared() const {
        return _prepared;
    }

    // If '_arr.has_value() && !_arr->isOwned()' is true, this method will make a copy of the BSON
    // and then update update '_arr' and '_elements' to point to the copied BSON instead of the
    // original BSON. If '_arr.has_value() && !_arr->isOwned()' is false, this method does nothing.
    // After this method returns, you are guaranteed that '!_arr.has_value() || _arr->isOwned()'
    // will be true.
    void makeBSONOwned();

    // This method is called by SBE to "prepare" this InListData for use in an SBE plan. Once
    // prepare() is called on an InListData, it can no longer be modified.
    void prepare();

    const BSONObj& getOwnedBSONStorage() const {
        tassert(8558800, "Expected BSON storage to be owned", isBSONOwned());
        return *_arr;
    }

private:
    InListData(const InListData& other);

    Status setElementsImpl(boost::optional<BSONObj> arr,
                           boost::optional<std::vector<BSONElement>> elementsIn,
                           bool errorOnRegex = true,
                           boost::optional<uint32_t> elemsSizeHint = boost::none,
                           const std::function<Status(const BSONElement&)>& fn = {});

    void updateSbeTagMasks();

    // This method will sort and de-dup the BSONElements in '_elements' if they haven't already
    // been sorted and de-duped.
    void sortAndDedupElements();

    void buildHashSet();

    // Collator used to construct the comparator for comparing elements.
    const CollatorInterface* _collator = nullptr;

    // Bitset that indicates which SBE TypeTags could potentially be equal to an element in
    // '_elements'.
    uint64_t _sbeTagMask = 0;
    uint64_t _hashSetSbeTagMask = 0;

    // Bitset that indicates which BSONTypes could potentially be equal to an element in
    // '_elements'.
    uint32_t _typeMask = 0;

    // Whether or not '_elements' has been initialized.
    bool _elementsInitialized = false;

    // Whether or not '_elements' contains an empty array.
    bool _hasEmptyArray = false;

    // Whether or not '_elements' contains an empty object.
    bool _hasEmptyObject = false;

    // Whether or not '_elements' contains a non-empty array.
    bool _hasNonEmptyArray = false;

    // Whether or not '_elements' contains a non-empty object.
    bool _hasNonEmptyObject = false;

    // Whether or not '_elements' contains one or more strings whose lengths (in bytes) exceed
    // 'kLargeStringThreshold'.
    bool _hasLargeStrings = false;

    // Boolean flags that track whether '_elements' is sorted, whether it's sorted and de-duped,
    // and whether it contains multiple unique elements (i.e. after de-duping it has more than
    // one element).
    bool _sorted = false;
    bool _sortedAndDeduped = false;
    bool _hasMultipleUniqueElements = false;

    // Whether or not '_hashSet' has been initialized.
    bool _hashSetInitialized = false;

    // Whether or not this InListData has been "prepared". Once an InListData transitions to the
    // "prepared" state, it cannot be modified and will remain in the "prepared" state for the
    // rest of its lifetime.
    bool _prepared = false;

    // An optional BSON array field. If this field is not boost::none, it will point to a BSON array
    // that contains all of the BSONElements from '_elements'.
    boost::optional<BSONObj> _arr;

    // An optional BSON array field. If the 'setElements(BSONObj)' method is called and then later
    // the 'setElements(vector<BSONElement>)' method is called, this field is used to keep the old
    // BSON array alive in case some of the elements in the 'vector<BSONElement>' point to it.
    boost::optional<BSONObj> _oldBackingArr;

    // If '_elementsInitialized' is true, then this field will contain the all elements (with regex
    // type values filtered out). If '_arr.has_value()' is false, this field will be empty.
    std::vector<BSONElement> _elements;

    // Optional vector of BSONElements. If the 'setElements(vector<BSONElement>)' method is called
    // and then 'sortAndDedupElements()' is called, this field is used to save a copy of '_elements'
    // prior to sorting and deduping.
    boost::optional<std::vector<BSONElement>> _originalElements;

    // De-duped hash set containing all non-string shallow-type elements and all NumberDecimal
    // elements. If _collator is null, this hash set will also contain all non-large strings/symbols
    // (i.e. strings and symbols whose length doesn't exceed 'kLargeStringThreshold').
    sbe::value::ValueSetType _hashSet;

    // This field indicates where the beginning of the binary search range should be when using
    // binary search as a fallback to searching '_hashSet'.
    size_t _binarySearchStartOffset = 0;
};
}  // namespace mongo
