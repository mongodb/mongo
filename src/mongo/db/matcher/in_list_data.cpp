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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/in_list_data.h"
#include "mongo/util/string_listset.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace mongo {
InListData::InListData(CloneCtorTag, const InListData& other)
    : _collator(other._collator),
      _sbeTagMask(other._sbeTagMask),
      _hashSetSbeTagMask(other._hashSetSbeTagMask),
      _typeMask(other._typeMask),
      _elementsInitialized(other._elementsInitialized),
      _hasEmptyArray(other._hasEmptyArray),
      _hasEmptyObject(other._hasEmptyObject),
      _hasNonEmptyArray(other._hasNonEmptyArray),
      _hasNonEmptyObject(other._hasNonEmptyObject),
      _hasLargeStrings(other._hasLargeStrings),
      _wasPreSorted(other._wasPreSorted),
      _wasPreSortedAndDeduped(other._wasPreSortedAndDeduped),
      _hasSingleUniqueElement(other._hasSingleUniqueElement),
      _arr(other._arr),
      _oldBackingArr(other._oldBackingArr),
      _originalElements(other._originalElements),
      _sortedElements(boost::in_place_init, cloneSortedElements(other._sortedElements)) {}

void InListData::appendElements(BSONArrayBuilder& bab, bool getSortedAndDeduped) {
    for (const auto& elem : getElements(getSortedAndDeduped)) {
        bab.append(elem);
    }
}

std::vector<BSONElement> InListData::getFirstOfEachType(bool getSortedAndDeduped) const {
    stdx::unordered_set<int> seenTypes;
    std::vector<BSONElement> result;

    for (const auto& elem : getElements(getSortedAndDeduped)) {
        if (seenTypes.insert(canonicalizeBSONType(elem.type())).second) {
            result.emplace_back(elem);
        }
    }

    return result;
}

Status InListData::setElementsImpl(boost::optional<BSONObj> arr,
                                   boost::optional<std::vector<BSONElement>> elementsIn,
                                   bool errorOnRegex,
                                   boost::optional<uint32_t> elemsSizeHint,
                                   const std::function<Status(const BSONElement&)>& fn) {
    tassert(7690405, "Cannot call setElementImpl() after InListData has been shared", !isShared());
    tassert(7690416,
            "Expected either 'arr' or 'elementsIn' to be defined but not both",
            arr.has_value() != elementsIn.has_value());

    bool elemsAreSorted = true;
    bool elemsAreUnique = true;
    bool hasMultipleUniqueElements = false;

    uint32_t typeMask = 0;
    bool hasEmptyArray = false;
    bool hasEmptyObject = false;
    bool hasNonEmptyArray = false;
    bool hasNonEmptyObject = false;
    bool hasLargeStrings = false;

    std::vector<BSONElement> elements;

    if (arr) {
        const uint32_t arrSizeInBytes = arr->objsize();

        if (elemsSizeHint && *elemsSizeHint > 0) {
            elements.reserve(*elemsSizeHint);
        } else {
            const size_t approxNumFields = arrSizeInBytes / 16u;
            elements.reserve(approxNumFields);
        }
    } else {
        elements = std::move(*elementsIn);
    }

    const bool appendToElementsVector = arr.has_value();

    // We define a lambda to process the elements, and we invoke the lambda with the appropriate
    // iterators.
    auto processElements = [&](auto&& it, auto&& itEnd) -> Status {
        boost::optional<BSONElement> prevElem;

        for (; it != itEnd; ++it) {
            BSONElement e = *it;
            auto type = e.type();

            if (fn) {
                auto status = fn(e);
                if (!status.isOK()) {
                    return status;
                }
            }

            if (type == BSONType::RegEx) {
                if (errorOnRegex) {
                    return Status(ErrorCodes::BadValue, "Cannot insert regex into InListData");
                } else {
                    continue;
                }
            }

            if (type == BSONType::Undefined) {
                return Status(ErrorCodes::BadValue, "Cannot insert undefined into InListData");
            }

            if (appendToElementsVector) {
                elements.emplace_back(e);
            }

            typeMask |= getBSONTypeMask(type);

            if (type == BSONType::String || type == BSONType::Symbol) {
                uint32_t len = e.valuestrsize() - 1;
                bool isLargeString = len > kLargeStringThreshold;
                hasLargeStrings |= isLargeString;
            } else if (type == BSONType::Array) {
                // If 'e' is an array, update 'hasEmptyArray' and 'hasNonEmptyArray'.
                hasEmptyArray |= e.Obj().isEmpty();
                hasNonEmptyArray |= !e.Obj().isEmpty();
            } else if (type == BSONType::Object) {
                // If 'e' is an object, update 'hasEmptyObject' and 'hasNonEmptyObject'.
                hasEmptyObject |= e.Obj().isEmpty();
                hasNonEmptyObject |= !e.Obj().isEmpty();
            }

            if (elemsAreSorted) {
                if (prevElem) {
                    auto result = prevElem->woCompare(e, kIgnoreFieldName, _collator);

                    if (result > 0) {
                        // If 'prevElem' is greater than 'e', then we know the elements are not
                        // sorted. Also, because the elements are not sorted, when this loop ends
                        // we won't have enough information to determine whether the elements are
                        // unique.
                        elemsAreSorted = false;
                        elemsAreUnique = false;
                        hasMultipleUniqueElements = true;
                    } else if (result == 0) {
                        // If 'prevElem' equals 'e', then we know the elements contain duplicates.
                        if (elemsAreUnique) {
                            elemsAreUnique = false;
                        }
                    } else {
                        if (!hasMultipleUniqueElements) {
                            hasMultipleUniqueElements = true;
                        }
                    }
                }

                prevElem = e;
            }
        }

        return Status::OK();
    };

    auto status = arr ? processElements(arr->begin(), arr->end())
                      : processElements(elements.begin(), elements.end());
    if (!status.isOK()) {
        return status;
    }

    // If 'typeMask' contains any numeric type, update 'typeMask' to contain all numeric types.
    uint32_t numberMask = getBSONTypeMask(BSONType::NumberInt) |
        getBSONTypeMask(BSONType::NumberLong) | getBSONTypeMask(BSONType::NumberDouble) |
        getBSONTypeMask(BSONType::NumberDecimal);
    if (typeMask & numberMask) {
        typeMask |= numberMask;
    }

    // If 'typeMask' contains any string-like type (string or symbol), update 'typeMask' to contain
    // all string-like types.
    uint32_t stringLikeMask = getBSONTypeMask(BSONType::String) | getBSONTypeMask(BSONType::Symbol);
    if (typeMask & stringLikeMask) {
        typeMask |= stringLikeMask;
    }

    // Save type info fields.
    _typeMask = typeMask;
    _hasEmptyArray = hasEmptyArray;
    _hasEmptyObject = hasEmptyObject;
    _hasNonEmptyArray = hasNonEmptyArray;
    _hasNonEmptyObject = hasNonEmptyObject;
    _hasLargeStrings = hasLargeStrings;

    // Update '_sbeTagMask' and '_hashSetSbeTagMask'.
    updateSbeTagMasks();

    if (arr) {
        // If 'arr' is defined, save 'arr' into '_arr' and clear '_oldBackingArr'.
        _oldBackingArr = boost::none;
        _arr = std::move(arr);
    } else {
        // If 'arr' is not defined, save the old value of '_arr' into '_oldBackingArr' if needed
        // and then clear '_arr'.
        if (_arr && _arr->isOwned()) {
            tassert(7690413, "Expected '_oldBackingArr' to be 'boost::none'", !_oldBackingArr);
            _oldBackingArr = std::move(_arr);
        }

        _arr = boost::none;
    }

    // Save 'elements' into '_originalElements', reset '_sortedElements' back to the "uninitialized"
    // state, and mark the elements as being initialized.
    _originalElements = std::move(elements);
    _sortedElements.emplace();
    _elementsInitialized = true;

    // Record what we observed about '_originalElements' regarding whether it's been pre-sorted
    // and whether it's been pre-deduplicated.
    _wasPreSorted = elemsAreSorted;
    _wasPreSortedAndDeduped = elemsAreSorted && elemsAreUnique;
    _hasSingleUniqueElement = !_originalElements.empty() && !hasMultipleUniqueElements;

    return Status::OK();
}

void InListData::updateSbeTagMasks() {
    static constexpr size_t numTypeTags = size_t(sbe::value::TypeTags::TypeTagsMax);

    static_assert(numTypeTags <= 64);

    _sbeTagMask = 0;
    _hashSetSbeTagMask = 0;

    for (uint8_t tagValue = 0; tagValue < numTypeTags; ++tagValue) {
        auto tag = static_cast<sbe::value::TypeTags>(tagValue);
        BSONType type = sbe::value::tagToType(tag);

        if (type != BSONType::EOO && (_typeMask & getBSONTypeMask(type))) {
            _sbeTagMask |= (1ull << tagValue);

            if ((sbe::value::isShallowType(tag) && !sbe::value::isStringOrSymbol(tag)) ||
                tag == sbe::value::TypeTags::NumberDecimal ||
                (sbe::value::isStringOrSymbol(tag) && !_collator)) {
                // If 'tag' is eligible to use the hash, set the corresponding bit in
                // '_hashSetSbeTagMask'.
                _hashSetSbeTagMask |= (1ull << tagValue);
            }
        }
    }
}

void InListData::setCollator(const CollatorInterface* coll) {
    tassert(7690407, "Cannot call setCollator() after InListData has been shared", !isShared());

    // Set '_collator'.
    auto oldColl = _collator;
    _collator = coll;

    // If setElements() hasn't been called yet or if 'coll' matches the old collator, then there
    // is no more work to do and we can return early.
    if (!_elementsInitialized || coll == oldColl ||
        CollatorInterface::collatorsMatch(coll, oldColl)) {
        return;
    }

    const uint32_t collatableTypesMask = getBSONTypeMask(BSONType::String) |
        getBSONTypeMask(BSONType::Symbol) | getBSONTypeMask(BSONType::Array) |
        getBSONTypeMask(BSONType::Object);

    // If _originalElements contains at least one collatable type, then we call setElementsImpl()
    // passing in the same BSON array or the same std::vector<BSONElement>. This will cause
    // '_originalElements' to be re-built and re-sorted using the new collation.
    if (_typeMask & collatableTypesMask) {
        if (_arr) {
            // '_arr' may contain Regexes. These Regexes are not part of the equalities list and in
            // this context we just want to ignore them, so we set 'errorOnRegex' to false.
            constexpr bool errorOnRegex = false;
            auto elemsSize = static_cast<uint32_t>(_originalElements.size());

            auto status = setElementsImpl(*_arr, {}, errorOnRegex, elemsSize);
            tassert(status);
        } else {
            auto originalElems = std::move(_originalElements);
            _originalElements = std::vector<BSONElement>{};

            auto status = setElementsImpl({}, std::move(originalElems));
            tassert(status);
        }
    }
}

void InListData::makeBSONOwned() {
    tassert(7690408, "Cannot call makeBSONOwned() after InListData has been shared", !isShared());

    // If setElements() hasn't been called yet or if isBSONOwned() is true, then there is
    // nothing to do.
    if (!_elementsInitialized || isBSONOwned()) {
        return;
    }

    // If '_arr' is defined, then we simply call makeArrOwned() and return.
    if (_arr) {
        makeArrOwned();
        return;
    }

    // If '_arr' is not defined, then we need to build a new BSONArray from _originalElements.
    BSONArrayBuilder bab;
    for (const auto& elem : _originalElements) {
        bab.append(elem);
    }
    auto arr = bab.obj();

    // Call setElementsImpl() and pass in the new BSONArray. This will set '_originalElements',
    // '_sortedElements', and all other fields appropriately.
    constexpr bool errorOnRegex = true;
    auto elemsSize = static_cast<uint32_t>(_originalElements.size());

    auto status = setElementsImpl(std::move(arr), {}, errorOnRegex, elemsSize);
    tassert(status);
}

void InListData::makeArrOwned() {
    if (!_arr || _arr->isOwned()) {
        return;
    }

    // Get a pointer to the old BSON buffer.
    const char* oldBuf = _arr->objdata();

    // Copy _arr's BSON data into a new owned buffer.
    _arr->makeOwned();
    const char* newBuf = _arr->objdata();

    // Update each BSONElement in '_originalElements' to refer to the new buffer.
    for (auto& e : _originalElements) {
        const char* newData = newBuf + (e.rawdata() - oldBuf);
        e = BSONElement(newData, e.fieldNameSize(), BSONElement::TrustedInitTag{});
    }

    // If '_sortedElements' holds a vector, update each BSONElement in '_sortedElements' to
    // refer to the new buffer.
    if (std::vector<BSONElement>* sortedElems = _sortedElements->getIfInitialized()) {
        for (auto& e : *sortedElems) {
            const char* newData = newBuf + (e.rawdata() - oldBuf);
            e = BSONElement(newData, e.fieldNameSize(), BSONElement::TrustedInitTag{});
        }
    }
}
}  // namespace mongo
