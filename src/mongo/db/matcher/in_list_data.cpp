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
InListData::InListData(const InListData& other)
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
      _sorted(other._sorted),
      _sortedAndDeduped(other._sortedAndDeduped),
      _hasMultipleUniqueElements(other._hasMultipleUniqueElements),
      _hashSetInitialized(false),
      _prepared(false),
      _arr(other._arr),
      _oldBackingArr(other._oldBackingArr),
      _elements(other._elements),
      _originalElements(other._originalElements),
      _hashSet(0, sbe::value::ValueHash{}, sbe::value::ValueEq{}) {}

void InListData::appendElements(BSONArrayBuilder& bab, bool getSortedAndDeduped) {
    if (getSortedAndDeduped) {
        for (size_t i = 0; i < _elements.size(); ++i) {
            bab.append(_elements[i]);
        }
    } else {
        appendOriginalElements(bab);
    }
}

void InListData::appendOriginalElements(BSONArrayBuilder& bab) const {
    if (_arr) {
        BSONObjIterator it(*_arr);
        while (it.more()) {
            BSONElement e = it.next();
            if (e.type() != BSONType::RegEx) {
                bab.append(e);
            }
        }
    } else {
        auto& elems = _originalElements ? *_originalElements : _elements;
        for (size_t i = 0; i < elems.size(); ++i) {
            bab.append(elems[i]);
        }
    }
}

std::vector<BSONElement> InListData::getFirstOfEachType(bool getSortedAndDeduped) {
    stdx::unordered_set<int> seenTypes;
    std::vector<BSONElement> result;

    if (getSortedAndDeduped) {
        for (auto&& elem : getElements()) {
            if (seenTypes.insert(canonicalizeBSONType(elem.type())).second) {
                result.emplace_back(elem);
            }
        }
    } else {
        if (_arr) {
            for (auto&& elem : *_arr) {
                // '_arr' might contain regexs. In this context, we ignore any regexes that we might
                // encounter.
                if (elem.type() != BSONType::RegEx) {
                    if (seenTypes.insert(canonicalizeBSONType(elem.type())).second) {
                        result.emplace_back(elem);
                    }
                }
            }
        } else {
            auto& elems = _originalElements ? *_originalElements : _elements;
            for (auto&& elem : elems) {
                if (seenTypes.insert(canonicalizeBSONType(elem.type())).second) {
                    result.emplace_back(elem);
                }
            }
        }
    }

    return result;
}

Status InListData::setElementsImpl(boost::optional<BSONObj> arr,
                                   boost::optional<std::vector<BSONElement>> elementsIn,
                                   bool errorOnRegex,
                                   boost::optional<uint32_t> elemsSizeHint,
                                   const std::function<Status(const BSONElement&)>& fn) {
    tassert(7690405, "Cannot call setElementImpl() after InListData has been prepared", !_prepared);
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
            } else {
                if (type == BSONType::Array) {
                    // If 'e' is an array, update 'hasEmptyArray' and 'hasNonEmptyArray'.
                    hasEmptyArray |= e.Obj().isEmpty();
                    hasNonEmptyArray |= !e.Obj().isEmpty();
                } else if (type == BSONType::Object) {
                    // If 'e' is an object, update 'hasEmptyObject' and 'hasNonEmptyObject'.
                    hasEmptyObject |= e.Obj().isEmpty();
                    hasNonEmptyObject |= !e.Obj().isEmpty();
                }
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

    // We defer on actually sorting and deduping '_elements'. Record what we've observed so far.
    _sorted = elemsAreSorted;
    _sortedAndDeduped = elemsAreSorted && elemsAreUnique;
    _hasMultipleUniqueElements = hasMultipleUniqueElements;

    if (arr) {
        // If 'arr' is defined, then save 'arr' and 'elements' into '_arr' and '_elements'
        // respectively and clear '_oldBackingArr' and '_originalElements'.
        _arr = std::move(arr);
        _oldBackingArr = boost::none;
        _elements = std::move(elements);
        _originalElements = boost::none;
    } else {
        // If 'arr' is not defined, save the old value of '_arr' into '_oldBackingArr' if needed,
        // save 'elements' into '_elements', and clear '_arr' and '_originalElements'.
        if (_arr && _arr->isOwned()) {
            tassert(7690413, "Expected '_oldBackingArr' to be 'boost::none'", !_oldBackingArr);
            _oldBackingArr = std::move(_arr);
        }
        _arr = boost::none;

        _elements = std::move(elements);
        _originalElements = boost::none;
    }

    // Mark the elements as being initialized.
    _elementsInitialized = true;

    // Save type info fields.
    _typeMask = typeMask;
    _hasEmptyArray = hasEmptyArray;
    _hasEmptyObject = hasEmptyObject;
    _hasNonEmptyArray = hasNonEmptyArray;
    _hasNonEmptyObject = hasNonEmptyObject;
    _hasLargeStrings = hasLargeStrings;

    // Update '_sbeTagMask' and '_hashSetSbeTagMask'.
    updateSbeTagMasks();

    // Sort and de-dup the elements.
    sortAndDedupElements();

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

void InListData::sortAndDedupElements() {
    if (_sortedAndDeduped) {
        return;
    }

    auto elemLt = InListElemLessThan(_collator);

    bool sorted = _sorted;
    bool sortedAndDeduped = _sortedAndDeduped;
    _sorted = true;
    _sortedAndDeduped = true;

    if (!sortedAndDeduped) {
        // Save a copy of the original '_elements' vector before sorting and de-duping.
        _originalElements.emplace(_elements);

        if (!sorted) {
            std::sort(_elements.begin(), _elements.end(), elemLt);
        }

        auto elemEq = InListElemEqualTo(_collator);
        auto newEnd = std::unique(_elements.begin(), _elements.end(), elemEq);
        if (newEnd != _elements.end()) {
            _elements.erase(newEnd, _elements.end());
        }
    }
}

void InListData::setCollator(const CollatorInterface* coll) {
    tassert(7690407, "Cannot call setCollator() after InListData has been prepared", !_prepared);

    // Set '_collator'.
    auto oldColl = _collator;
    _collator = coll;

    // If setElements() hasn't been called yet or if 'coll' matches the old collator, then there
    // is no more work to do and we can return early.
    if (!_elementsInitialized || coll == oldColl ||
        CollatorInterface::collatorsMatch(coll, oldColl)) {
        return;
    }

    // If _elements contains at least one collatable type, then we call setElementsImpl() with the
    // same BSON array to force '_elements' to be re-built and sorted using the new collation.
    const uint32_t collatableTypesMask = getBSONTypeMask(BSONType::String) |
        getBSONTypeMask(BSONType::Symbol) | getBSONTypeMask(BSONType::Array) |
        getBSONTypeMask(BSONType::Object);

    if (_typeMask & collatableTypesMask) {
        if (_arr) {
            // '_arr' may contain Regexes. These Regexes are not part of the equalities list and in
            // this context we just want to ignore them, so we set 'errorOnRegex' to false.
            constexpr bool errorOnRegex = false;
            auto elemsSize = static_cast<uint32_t>(_elements.size());

            auto status = setElementsImpl(*_arr, {}, errorOnRegex, elemsSize);
            tassert(status);
        } else {
            auto status = _originalElements ? setElementsImpl({}, std::move(*_originalElements))
                                            : setElementsImpl({}, std::move(_elements));
            tassert(status);
        }
    }
}

void InListData::makeBSONOwned() {
    tassert(7690408, "Cannot call makeBSONOwned() after InListData has been prepared", !_prepared);

    // If setElements() hasn't been called yet or if isBSONOwned() is true, then there's nothing
    // to do.
    if (!_elementsInitialized || isBSONOwned()) {
        return;
    }

    if (_arr) {
        // Get a pointer to the old BSON buffer.
        const char* oldBuf = _arr->objdata();

        // Copy _arr's BSON data into a new owned buffer.
        _arr->makeOwned();
        const char* newBuf = _arr->objdata();

        // Update each BSONElement in '_elements' to refer to the new buffer.
        for (auto&& e : _elements) {
            const char* newData = newBuf + (e.rawdata() - oldBuf);
            e = BSONElement(newData, e.fieldNameSize(), BSONElement::TrustedInitTag{});
        }
    } else {
        // Serialize the original list of elements into an owned BSONArray.
        BSONArrayBuilder bab;
        auto& elems = _originalElements ? *_originalElements : _elements;
        for (size_t i = 0; i < elems.size(); ++i) {
            bab.append(elems[i]);
        }
        auto arr = bab.obj();

        // Call setElementsImpl() and pass in the owned BSONArray.
        constexpr bool errorOnRegex = true;
        auto elemsSize = static_cast<uint32_t>(elems.size());

        auto status = setElementsImpl(std::move(arr), {}, errorOnRegex, elemsSize);
        tassert(status);
    }
}

void InListData::prepare() {
    tassert(7690409, "Cannot call prepare() when InListData has already been prepared", !_prepared);

    buildHashSet();

    _prepared = true;
}

void InListData::buildHashSet() {
    if (_hashSetInitialized) {
        return;
    }

    for (auto&& e : _elements) {
        auto [tag, val] = sbe::bson::convertFrom<true>(e);

        if (e.type() == BSONType::String || e.type() == BSONType::Symbol) {
            auto str = e.valueStringData();

            if (!_collator && str.size() <= kLargeStringThreshold) {
                _hashSet.insert({tag, val});
            }
        } else if (sbe::value::isShallowType(tag) || tag == sbe::value::TypeTags::NumberDecimal) {
            _hashSet.insert({tag, val});
        }
    }

    // Use lower_bound() / upper_bound() to find the first element that must be inside the
    // "search range" that should be used when the contains() method performs a binary search.
    auto elemLt = InListElemLessThan(_collator);
    auto binarySearchBeginIt = (!_collator && !_hasLargeStrings)
        ? std::upper_bound(_elements.begin(), _elements.end(), BSONType::String, elemLt)
        : std::lower_bound(_elements.begin(), _elements.end(), BSONType::String, elemLt);

    _binarySearchStartOffset = binarySearchBeginIt - _elements.begin();

    _hashSetInitialized = true;
}
}  // namespace mongo
