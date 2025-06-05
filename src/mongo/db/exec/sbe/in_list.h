/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/matcher/in_list_data.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mongo::sbe {
/**
 * InList is a structure that is built from an "in-list" which is represented in the form of
 * an InListData object.
 *
 * An InList can be used by an SBE plan at run time to check if a given value is found in the
 * the in-list's set of elements.
 *
 * InList employs a hybrid strategy that makes use of both a hash set and binary search.
 *
 * The in-list's elements are partitioned based on type. Non-string shallow types and NumberDecimals
 * are put into a hash set, and if collator is null then all strings/symbols whose length does not
 * exceed 'kLargeStringThreshold' are added to the hash set as well. All other types are excluded
 * from the hash set.
 *
 * When contains() is called, based on the search value's type and based on the types present in the
 * in-list's elements, the contains() method will decide whether it should search the hash set or
 * whether it should use binary search (or whether it should do neither of these and return false).
 */
class InList {
public:
    static constexpr size_t kLargeStringThreshold = InListData::kLargeStringThreshold;

    InList(std::shared_ptr<const InListData> inListData,
           boost::optional<const CollatorInterface*> coll = boost::none)
        : _sbeTagMask(inListData->getSbeTagMask()),
          _hashSetSbeTagMask(inListData->getHashSetSbeTagMask()),
          _hasLargeStrings(inListData->hasLargeStrings()),
          _hashSet(0, value::ValueHash{}, value::ValueEq{}),
          _collator(coll ? *coll : inListData->getCollator()),
          _elemLt(_collator),
          _inListData(std::move(inListData)) {
        // Determine if we might need to use binary search for some inputs.
        bool useBinarySearchForSomeInputs = _sbeTagMask != _hashSetSbeTagMask || _hasLargeStrings;

        // If we need the sorted elements (for binary search) or if the elements have already been
        // sorted, then we use the sorted elements here, otherwise we use the original elements.
        bool getSortedAndDeduped =
            useBinarySearchForSomeInputs || _inListData->elementsHaveBeenSorted();
        const auto& elems = _inListData->getElements(getSortedAndDeduped);

        // Initialize '_elementsBegin' and '_elementsEnd'.
        _elementsBegin = elems.data();
        _elementsEnd = elems.data() + elems.size();

        // If we might need to use binary search for some inputs, then we need to initialize
        // '_elementsSearchBegin'.
        if (useBinarySearchForSomeInputs) {
            // Use lower_bound() / upper_bound() to find the beginning of the range that should be
            // used for binary search.
            _elementsSearchBegin = (!_collator && !_hasLargeStrings)
                ? std::upper_bound(_elementsBegin, _elementsEnd, BSONType::string, _elemLt)
                : std::lower_bound(_elementsBegin, _elementsEnd, BSONType::string, _elemLt);
        }

        // Populate '_hashSet' if there are any elements that are eligible to use the hash.
        if (_hashSetSbeTagMask != 0) {
            for (const auto& elem : elems) {
                auto [tag, val] = bson::convertFrom<true>(elem);

                if (isNonStringShallowType(tag) || tag == value::TypeTags::NumberDecimal ||
                    ((elem.type() == BSONType::string || elem.type() == BSONType::symbol) &&
                     !_collator && elem.valueStringData().size() <= kLargeStringThreshold)) {
                    // If 'tag' is a non-string shallow type, -OR- if 'tag' is NumberDecimal,
                    // -OR- if '_collator' is null and '{tag, val}' is a string/symbol whose
                    // length (in bytes) does not exceed 'kLargeStringThreshold', then we
                    // should insert '{tag, val}' into '_hashSet'.
                    _hashSet.insert({tag, val});
                }
            }
        }
    }

    // Returns true if the specified SBE tag/value is equal to one of this InList's elements,
    // otherwise returns false.
    bool contains(value::TypeTags tag, value::Value val) const {
        constexpr uint64_t stringOrSymbolSbeTagMask =
            (1ull << static_cast<uint64_t>(value::TypeTags::StringSmall)) |
            (1ull << static_cast<uint64_t>(value::TypeTags::StringBig)) |
            (1ull << static_cast<uint64_t>(value::TypeTags::bsonString)) |
            (1ull << static_cast<uint64_t>(value::TypeTags::bsonSymbol));

        if ((_hashSetSbeTagMask & stringOrSymbolSbeTagMask) == 0 ||
            ((1ull << static_cast<uint64_t>(tag)) & stringOrSymbolSbeTagMask) == 0) {
            // If the hash set doesn't contain strings or symbols -OR- if 'tag' is not a
            // string or symbol, then check if 'tag' is a non-string shallow type or
            // NumberDecimal.
            if (isNonStringShallowType(tag) || tag == value::TypeTags::NumberDecimal) {
                // If 'tag' is a non-string shallow type or NumberDecimal, then search the hash set.
                return _hashSet.find({tag, val}) != _hashSet.end();
            } else if (((1ull << static_cast<uint64_t>(tag)) & _sbeTagMask) != 0) {
                // Otherwise, if 'tag' is present in '_sbeTagMask', we use binary search.
                return binarySearch(tag, val);
            }
        } else {
            // If the hash set contains at least one string or symbol -AND- if 'tag' is a string
            // or symbol, then check if the length returned by 'getStringOrSymbolLength(tag, val)'
            // exceeds the threshold.
            if (value::getStringOrSymbolLength(tag, val) <= kLargeStringThreshold) {
                // If the length does not exceed the threshold, then search the hash set.
                return _hashSet.find({tag, val}) != _hashSet.end();
            } else if (_hasLargeStrings) {
                // Otherwise, if the hash set contains at least one string or symbol whose length
                // exceeds the threshold, we use binary search.
                return binarySearch(tag, val);
            }
        }
        // If we didn't match any of the conditions above, then that means that tag/val is not
        // present in this InList, so we return false.
        return false;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    template <typename StreamT>
    void writeToStream(StreamT& stream) const {
        bool first = true;
        for (auto* elem = _elementsBegin; elem != _elementsEnd; ++elem) {
            if (first) {
                first = false;
            } else {
                stream << " ";
            }

            stream << elem->toString(false);
        }
    }

private:
    MONGO_COMPILER_ALWAYS_INLINE
    static constexpr bool isNonStringShallowType(value::TypeTags tag) noexcept {
        return value::isShallowType(tag) && tag != value::TypeTags::StringSmall;
    }

    MONGO_COMPILER_ALWAYS_INLINE
    bool binarySearch(value::TypeTags tag, value::Value val) const {
        return std::binary_search(_elementsSearchBegin, _elementsEnd, std::pair(tag, val), _elemLt);
    }

    // Bitset that indicates which SBE TypeTags could potentially be equal to an element in
    // this InList.
    uint64_t _sbeTagMask = 0;
    uint64_t _hashSetSbeTagMask = 0;

    // Whether or not this InList contains one or more strings whose lengths (in bytes) exceed
    // 'kLargeStringThreshold'.
    bool _hasLargeStrings = false;

    // De-duped hash set containing all non-string shallow-type elements and all NumberDecimal
    // elements. If _collator is null, this hash set will also contain all strings/symbols whose
    // length (in bytes) does not exceed 'kLargeStringThreshold'.
    value::ValueSetType _hashSet;

    // A contiguous array of BSONElements that represents all the values in this InList.
    const BSONElement* _elementsBegin = nullptr;
    const BSONElement* _elementsEnd = nullptr;

    // The beginning of the range to use for binary search. (The end of the range to use for
    // binary search is equal to '_elementsEnd'.)
    //
    // Note that this field may be null in cases where its been determined that binary search
    // will never need to be used for any input.
    const BSONElement* _elementsSearchBegin = nullptr;

    // Collator used by '_elemLt'.
    const CollatorInterface* _collator = nullptr;

    // Comparator used for comparing BSONElements.
    InListElemLessThan _elemLt;

    // A reference to the InListData object that owns the BSONElement vectors that '_elementsBegin',
    // '_elementsSearchBegin', and '_elementsEnd' point to. This InListData object may or may not
    // own the BSON that these BSONElements point to as well.
    //
    // The sole purpose of this field is to keep these BSONElement vectors alive (and possibly to
    // keep the BSON alive that these vectors point to as well).
    //
    // This field should _not_ be used to retrieve information about the InList or to search the
    // InList or perform any other operations, since the InListData object may contain pointers
    // to Collator objects or other memory that has since been reclaimed.
    std::shared_ptr<const InListData> _inListData;
};
}  // namespace mongo::sbe
