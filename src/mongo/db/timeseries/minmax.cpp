/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/timeseries/minmax.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/update/document_diff_serialization.h"

namespace mongo::timeseries {
namespace {
constexpr int32_t kMaxLinearSearchLength = 32;
constexpr StringData kArrayFieldName =
    "\0"_sd;  // Use a string that is illegal to represent fields in BSON
}  // namespace

MinMaxStore::Type MinMaxStore::Data::type() const {
    return _type;
}

/**
 * Flag to indicate if this MinMax::Data was updated since last clear.
 */
bool MinMaxStore::Data::updated() const {
    return _updated;
}

/**
 * Clear update flag.
 */
void MinMaxStore::Data::clearUpdated() {
    _updated = false;
}

BSONElement MinMaxStore::Data::value() const {
    return BSONElement(_value.buffer.get(), 1, _value.size, BSONElement::CachedSizeTag{});
}
BSONType MinMaxStore::Data::valueType() const {
    return (BSONType)_value.buffer[0];
}

void MinMaxStore::Data::setUnset() {
    _type = Type::kUnset;
    _updated = false;
}

void MinMaxStore::Data::setObject() {
    _type = Type::kObject;
    _updated = true;
}

void MinMaxStore::Data::setArray() {
    _type = Type::kArray;
    _updated = true;
}

void MinMaxStore::Data::setValue(const BSONElement& elem) {
    auto requiredSize = elem.size() - elem.fieldNameSize() + 1;
    if (_value.size < requiredSize) {
        _value.buffer = std::make_unique<char[]>(requiredSize);
    }
    // Store element as BSONElement buffer but strip out the field name
    _value.buffer[0] = elem.type();
    _value.buffer[1] = '\0';
    memcpy(_value.buffer.get() + 2, elem.value(), elem.valuesize());
    _value.size = requiredSize;
    _type = Type::kValue;
    _updated = true;
}

StringData MinMaxStore::Element::fieldName() const {
    return _fieldName;
}

bool MinMaxStore::Element::isArrayFieldName() const {
    return _fieldName == kArrayFieldName;
}

void MinMaxStore::Element::claimArrayFieldNameForObject(std::string name) {
    invariant(isArrayFieldName());
    _fieldName = std::move(name);
}

MinMaxStore::Data& MinMaxStore::Element::min() {
    return _min;
}

const MinMaxStore::Data& MinMaxStore::Element::min() const {
    return _min;
}

MinMaxStore::Data& MinMaxStore::Element::max() {
    return _max;
}

const MinMaxStore::Data& MinMaxStore::Element::max() const {
    return _max;
}

MinMaxStore::Iterator::Iterator(MinMaxStore::Entries::iterator pos) : _pos(pos) {}

MinMaxStore::Iterator::pointer MinMaxStore::Iterator::operator->() {
    return &_pos->_element;
}
MinMaxStore::Iterator::reference MinMaxStore::Iterator::operator*() {
    return _pos->_element;
}

MinMaxStore::Iterator& MinMaxStore::Iterator::operator++() {
    _pos += _pos->_offsetEnd;
    return *this;
}

bool MinMaxStore::Iterator::operator==(const MinMaxStore::Iterator& rhs) const {
    return _pos == rhs._pos;
}

bool MinMaxStore::Iterator::operator!=(const MinMaxStore::Iterator& rhs) const {
    return !operator==(rhs);
}

MinMaxStore::ConstIterator::ConstIterator(MinMaxStore::Entries::const_iterator pos) : _pos(pos) {}

MinMaxStore::ConstIterator::pointer MinMaxStore::ConstIterator::operator->() const {
    return &_pos->_element;
}
MinMaxStore::ConstIterator::reference MinMaxStore::ConstIterator::operator*() const {
    return _pos->_element;
}

MinMaxStore::ConstIterator& MinMaxStore::ConstIterator::operator++() {
    _pos += _pos->_offsetEnd;
    return *this;
}

bool MinMaxStore::ConstIterator::operator==(const ConstIterator& rhs) const {
    return _pos == rhs._pos;
}

bool MinMaxStore::ConstIterator::operator!=(const MinMaxStore::ConstIterator& rhs) const {
    return !operator==(rhs);
}

MinMaxStore::Obj::Obj(MinMaxStore::Entries& entries, MinMaxStore::Entries::iterator pos)
    : _entries(entries), _pos(pos) {}


MinMaxStore::Obj& MinMaxStore::Obj::operator=(const MinMaxStore::Obj& rhs) {
    if (this != &rhs) {
        _pos = rhs._pos;
    }
    return *this;
}

MinMaxStore::Obj MinMaxStore::Obj::object(MinMaxStore::Iterator pos) const {
    return {_entries, pos._pos};
}

MinMaxStore::Obj MinMaxStore::Obj::parent() const {
    return {_entries, _pos - _pos->_offsetParent};
}

MinMaxStore::Iterator MinMaxStore::Obj::iterator() const {
    return {_pos};
}

MinMaxStore::Element& MinMaxStore::Obj::element() {
    return _pos->_element;
}

const MinMaxStore::Element& MinMaxStore::Obj::element() const {
    return _pos->_element;
}

MinMaxStore::Iterator MinMaxStore::Obj::search(MinMaxStore::Iterator first,
                                               MinMaxStore::Iterator last,
                                               StringData fieldName) {
    // Use fast lookup if available
    if (_pos->_fieldNameToIndex) {
        auto it = _pos->_fieldNameToIndex->find(fieldName);
        if (it == _pos->_fieldNameToIndex->end()) {
            return last;
        }

        return {_pos + it->second};
    }

    // Perform linear search forward
    int remainingLinearSearch = kMaxLinearSearchLength;
    for (; first != last && remainingLinearSearch != 0; ++first, --remainingLinearSearch) {
        // Entry found.
        if (first->fieldName() == fieldName) {
            return first;
        }

        // Found entry that is used for an Array, we can claim this field.
        if (first->isArrayFieldName()) {
            first->claimArrayFieldNameForObject(fieldName.toString());
            return first;
        }
    }

    // Return if we reached our end when doing linear search
    if (first == last) {
        return last;
    }

    // We've exhausted the linear search limit, create a map to speedup future searches. Populate it
    // will all current subelements.
    _pos->_fieldNameToIndex = std::make_unique<StringMap<uint32_t>>();
    auto it = begin();
    auto itEnd = end();
    for (; it != itEnd; ++it) {
        (*_pos->_fieldNameToIndex)[it->fieldName().toString()] = it._pos->_offsetParent;
    }

    // Retry the search now when the map is created
    return search(first, last, fieldName);
}

MinMaxStore::Iterator MinMaxStore::Obj::search(MinMaxStore::Iterator first, StringData fieldName) {
    return search(first, end(), fieldName);
}

std::pair<MinMaxStore::Iterator, MinMaxStore::Iterator> MinMaxStore::Obj::insert(
    MinMaxStore::Iterator pos, std::string fieldName) {
    // Remember our iterator position so we can restore it after inserting a new element
    auto index = std::distance(_entries.begin(), _pos);
    auto inserted = _entries.emplace(pos._pos);
    _pos = _entries.begin() + index;

    // Setup our newly created entry
    inserted->_offsetEnd = 1;  // no subelements
    inserted->_element._fieldName = std::move(fieldName);
    inserted->_offsetParent = std::distance(_pos, inserted);

    // Also store our offset in the fast lookup map if it is available.
    if (_pos->_fieldNameToIndex) {
        _pos->_fieldNameToIndex->emplace(inserted->_element._fieldName, inserted->_offsetParent);
    }

    // We need to traverse the hiearchy up to the root and modify stored offsets to account for
    // this newly created entry. All entries with subobjects got their end-offset pushed by one.
    // All siblings after this entry got their offset to the parent pushed by one.
    auto it = inserted;
    auto parent = _pos;

    // Root object has "self" as parent
    while (it != parent) {
        ++parent->_offsetEnd;

        auto next = std::next(Iterator(it));
        auto end = Iterator(parent + parent->_offsetEnd);
        for (; next != end; ++next) {
            ++next._pos->_offsetParent;
            if (parent->_fieldNameToIndex) {
                ++parent->_fieldNameToIndex->at(next._pos->_element.fieldName());
            }
        }

        it = parent;
        parent = parent - parent->_offsetParent;
    }

    return std::make_pair(Iterator(inserted), end());
}

MinMaxStore::Iterator MinMaxStore::Obj::begin() {
    return {_pos + 1};
}

MinMaxStore::Iterator MinMaxStore::Obj::end() {
    return {_pos + _pos->_offsetEnd};
}

MinMaxStore::ConstIterator MinMaxStore::Obj::begin() const {
    return {_pos + 1};
}

MinMaxStore::ConstIterator MinMaxStore::Obj::end() const {
    return {_pos + _pos->_offsetEnd};
}

MinMaxStore::MinMaxStore() {
    auto& entry = entries.emplace_back();
    entry._offsetEnd = 1;
    entry._offsetParent = 0;
    entry._element._min._type = Type::kObject;
    entry._element._max._type = Type::kObject;
}

template <typename SkipFieldFn>
void MinMax::_updateObj(MinMaxStore::Obj& obj,
                        const BSONObj& doc,
                        bool updateMin,
                        bool updateMax,
                        const StringData::ComparatorInterface* stringComparator,
                        SkipFieldFn skipFieldFn) {
    auto it = obj.begin();
    auto end = obj.end();
    int allHandledOffset = 0;
    bool skipped = false;
    for (auto&& elem : doc) {
        StringData fieldName = elem.fieldNameStringData();
        if (skipFieldFn(fieldName)) {
            continue;
        }

        if (it == end && skipped) {
            // If we are at end but have skipped elements along the way we need to go back and
            // search in the skipped elements. We do not have to search over the consecutive range
            // in the beginning of elements already handled.
            auto begin = obj.begin();
            std::advance(begin, allHandledOffset);
            it = obj.search(begin, fieldName);
        }

        if (it == end) {
            // Field missing, we need to insert it at the end so we preserve the input field order.
            std::tie(it, end) = obj.insert(it, fieldName.toString());
        } else if (it->isArrayFieldName()) {
            // Entry is only used for Arrays, we can claim the field name.
            it->claimArrayFieldNameForObject(fieldName.toString());
        } else if (it->fieldName() != fieldName) {
            // Traversing the MinMax structure in lock-step with the input document resulted in
            // a miss. This means one of two things. (1) input document do not contain all
            // metadata fields and we need to skip over missing ones. Or (2) input document has
            // a different internal field order than previous inserts. We begin by searching
            // forward to see if we are case (1).
            auto found = obj.search(std::next(it), fieldName);
            if (found == end) {
                // Field not found. We can either be case (2) or this is a new field that need
                // to be inserted.
                if (skipped) {
                    // Search over the skipped elements in case we are case (2)
                    auto begin = obj.begin();
                    std::advance(begin, allHandledOffset);
                    found = obj.search(begin, it, fieldName);
                    if (found == it) {
                        // Still not found, insert the new field. Location doesn't matter much
                        // as we operating on incoming documents of different field orders.
                        // Select the point we know furthest back.
                        std::tie(it, end) = obj.insert(it, fieldName.toString());
                    } else {
                        it = found;
                    }
                } else {
                    // All previous elements have been found as we have never skipped, proceed
                    // with inserting this new field.
                    std::tie(it, end) = obj.insert(it, fieldName.toString());
                }
            } else {
                it = found;
                skipped = true;
            }
        }

        // It points to either a found existing or a newly inserted entry at this point. Recursively
        // update it.
        std::tie(it, end) = _update(obj.object(it), elem, updateMin, updateMax, stringComparator);

        // Re-construct obj from the returned iterator. If an insert was performed inside
        // _update it would dangle.
        obj = obj.object(it).parent();

        // Advance iterator and advance the all handled offset if we have not skipped anything.
        ++it;
        if (!skipped) {
            ++allHandledOffset;
        }
    }
}

void MinMax::update(const BSONObj& doc,
                    boost::optional<StringData> metaField,
                    const StringData::ComparatorInterface* stringComparator) {
    auto obj = _store.root();
    _updateObj(obj, doc, true, true, stringComparator, [&metaField](StringData fieldName) {
        return metaField && fieldName == metaField;
    });
}

std::pair<MinMaxStore::Iterator, MinMaxStore::Iterator> MinMax::_update(
    MinMaxStore::Obj obj,
    BSONElement elem,
    bool updateMinValues,
    bool updateMaxValues,
    const StringData::ComparatorInterface* stringComparator) {
    auto typeComp = [&](BSONType type) {
        return elem.canonicalType() - canonicalizeBSONType(type);
    };

    if (elem.type() == Object) {
        auto shouldUpdateObject = [&](MinMaxStore::Data& data, auto comp) {
            return data.type() == MinMaxStore::Type::kObject ||
                data.type() == MinMaxStore::Type::kUnset ||
                (data.type() == MinMaxStore::Type::kArray && comp(typeComp(Array), 0)) ||
                (data.type() == MinMaxStore::Type::kValue && comp(typeComp(data.valueType()), 0));
        };
        bool updateMin =
            updateMinValues && shouldUpdateObject(obj.element().min(), std::less<int>{});
        if (updateMin) {
            _setTypeObject(obj, GetMin{});
        }
        bool updateMax =
            updateMaxValues && shouldUpdateObject(obj.element().max(), std::greater<int>{});
        if (updateMax) {
            _setTypeObject(obj, GetMax{});
        }

        // Compare objects element-wise if min or max need to be updated
        if (updateMin || updateMax) {
            _updateObj(
                obj, elem.Obj(), updateMin, updateMax, stringComparator, [](StringData fieldName) {
                    return false;
                });
        }
        return {obj.iterator(), obj.parent().end()};
    }

    if (elem.type() == Array) {
        auto shouldUpdateArray = [&](MinMaxStore::Data& data, auto comp) {
            return data.type() == MinMaxStore::Type::kArray ||
                data.type() == MinMaxStore::Type::kUnset ||
                (data.type() == MinMaxStore::Type::kObject && comp(typeComp(Object), 0)) ||
                (data.type() == MinMaxStore::Type::kValue && comp(typeComp(data.valueType()), 0));
        };
        bool updateMin =
            updateMinValues && shouldUpdateArray(obj.element().min(), std::less<int>{});
        if (updateMin) {
            _setTypeArray(obj, GetMin{});
        }
        bool updateMax =
            updateMaxValues && shouldUpdateArray(obj.element().max(), std::greater<int>{});
        if (updateMax) {
            _setTypeArray(obj, GetMax{});
        }
        // Compare objects element-wise if min or max need to be updated
        if (updateMin || updateMax) {
            // Use Obj() instead of Array() to avoid instantiating a temporary std::vector
            auto elemArray = elem.Obj();
            auto elemIt = elemArray.begin();
            auto elemEnd = elemArray.end();

            auto it = obj.begin();
            auto end = obj.end();
            for (; elemIt != elemEnd; ++elemIt) {
                if (it == end)
                    std::tie(it, end) = obj.insert(it, kArrayFieldName.toString());

                std::tie(it, end) =
                    _update(obj.object(it), *elemIt, updateMin, updateMax, stringComparator);
                obj = obj.object(it).parent();
                ++it;
            }
        }
        return {obj.iterator(), obj.parent().end()};
    }

    auto maybeUpdateValue = [&](MinMaxStore::Data& data, auto comp) {
        if (data.type() == MinMaxStore::Type::kUnset ||
            (data.type() == MinMaxStore::Type::kObject && comp(typeComp(Object), 0)) ||
            (data.type() == MinMaxStore::Type::kArray && comp(typeComp(Array), 0)) ||
            (data.type() == MinMaxStore::Type::kValue &&
             comp(elem.woCompare(data.value(), false, stringComparator), 0))) {
            data.setValue(elem);
        }
    };
    if (updateMinValues) {
        maybeUpdateValue(obj.element().min(), std::less<>{});
    }

    if (updateMaxValues) {
        maybeUpdateValue(obj.element().max(), std::greater<>{});
    }

    return {obj.iterator(), obj.parent().end()};
}

BSONObj MinMax::min() {
    BSONObjBuilder builder;
    _append(_store.root(), &builder, GetMin());
    return builder.obj();
}

BSONObj MinMax::max() {
    BSONObjBuilder builder;
    _append(_store.root(), &builder, GetMax());
    return builder.obj();
}

template <typename GetDataFn>
void MinMax::_append(MinMaxStore::Obj obj, BSONObjBuilder* builder, GetDataFn getData) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const auto& data = getData(*it);
        if (data.type() == MinMaxStore::Type::kValue) {
            builder->appendAs(data.value(), it->fieldName());
        } else if (data.type() == MinMaxStore::Type::kObject) {
            BSONObjBuilder subObj(builder->subobjStart(it->fieldName()));
            _append(obj.object(it), &subObj, getData);
        } else if (data.type() == MinMaxStore::Type::kArray) {
            BSONArrayBuilder subArr(builder->subarrayStart(it->fieldName()));
            _append(obj.object(it), &subArr, getData);
        }
        if (data.updated())
            _clearUpdated(it, getData);
    }
}

template <typename GetDataFn>
void MinMax::_append(MinMaxStore::Obj obj, BSONArrayBuilder* builder, GetDataFn getData) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const auto& data = getData(*it);
        if (data.type() == MinMaxStore::Type::kValue) {
            builder->append(data.value());
        } else if (data.type() == MinMaxStore::Type::kObject) {
            BSONObjBuilder subObj(builder->subobjStart());
            _append(obj.object(it), &subObj, getData);
        } else if (data.type() == MinMaxStore::Type::kArray) {
            BSONArrayBuilder subArr(builder->subarrayStart());
            _append(obj.object(it), &subArr, getData);
        }
        if (data.updated())
            _clearUpdated(it, getData);
    }
}

BSONObj MinMax::minUpdates() {
    BSONObjBuilder builder;
    [[maybe_unused]] auto appended = _appendUpdates(_store.root(), &builder, GetMin());
    return builder.obj();
}

BSONObj MinMax::maxUpdates() {
    BSONObjBuilder builder;
    [[maybe_unused]] auto appended = _appendUpdates(_store.root(), &builder, GetMax());
    return builder.obj();
}

template <typename GetDataFn>
bool MinMax::_appendUpdates(MinMaxStore::Obj obj, BSONObjBuilder* builder, GetDataFn getData) {
    const auto& data = getData(obj.element());
    invariant(data.type() == MinMaxStore::Type::kObject ||
              data.type() == MinMaxStore::Type::kArray);

    bool appended = false;
    if (data.type() == MinMaxStore::Type::kObject) {
        bool hasUpdateSection = false;
        BSONObjBuilder updateSection;
        StringMap<BSONObj> subDiffs;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const auto& subdata = getData(*it);
            if (subdata.updated()) {
                if (subdata.type() == MinMaxStore::Type::kObject) {
                    BSONObjBuilder subObj(updateSection.subobjStart(it->fieldName()));
                    _append(obj.object(it), &subObj, getData);
                } else if (subdata.type() == MinMaxStore::Type::kArray) {
                    BSONArrayBuilder subArr(updateSection.subarrayStart(it->fieldName()));
                    _append(obj.object(it), &subArr, getData);
                } else {
                    updateSection.appendAs(subdata.value(), it->fieldName());
                }
                _clearUpdated(it, getData);
                appended = true;
                hasUpdateSection = true;
            } else if (subdata.type() != MinMaxStore::Type::kValue &&
                       subdata.type() != MinMaxStore::Type::kUnset) {
                BSONObjBuilder subDiff;
                if (_appendUpdates(obj.object(it), &subDiff, getData)) {
                    // An update occurred at a lower level, so append the sub diff.
                    subDiffs[doc_diff::kSubDiffSectionFieldPrefix + std::string(it->fieldName())] =
                        subDiff.obj();
                    appended = true;
                };
            }
        }
        if (hasUpdateSection) {
            builder->append(doc_diff::kUpdateSectionFieldName, updateSection.done());
        }

        // Sub diffs are required to come last.
        for (auto& subDiff : subDiffs) {
            builder->append(subDiff.first, std::move(subDiff.second));
        }
    } else {
        builder->append(doc_diff::kArrayHeader, true);
        DecimalCounter<size_t> count;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const auto& subdata = getData(*it);
            if (subdata.updated()) {
                std::string updateFieldName = str::stream()
                    << doc_diff::kUpdateSectionFieldName << StringData(count);
                if (subdata.type() == MinMaxStore::Type::kObject) {
                    BSONObjBuilder subObj(builder->subobjStart(updateFieldName));
                    _append(obj.object(it), &subObj, getData);
                } else if (subdata.type() == MinMaxStore::Type::kArray) {
                    BSONArrayBuilder subArr(builder->subarrayStart(updateFieldName));
                    _append(obj.object(it), &subArr, getData);
                } else {
                    builder->appendAs(subdata.value(), updateFieldName);
                }
                _clearUpdated(it, getData);
                appended = true;
            } else if (subdata.type() != MinMaxStore::Type::kValue &&
                       subdata.type() != MinMaxStore::Type::kUnset) {
                BSONObjBuilder subDiff;
                if (_appendUpdates(obj.object(it), &subDiff, getData)) {
                    // An update occurred at a lower level, so append the sub diff.
                    builder->append(str::stream() << doc_diff::kSubDiffSectionFieldPrefix
                                                  << StringData(count),
                                    subDiff.done());
                    appended = true;
                }
            }
            ++count;
        }
    }

    return appended;
}

template <typename GetDataFn>
void MinMax::_clearUpdated(MinMaxStore::Iterator elem, GetDataFn getData) {
    auto& data = getData(*elem);

    data.clearUpdated();
    if (data.type() == MinMaxStore::Type::kObject || data.type() == MinMaxStore::Type::kArray) {
        auto obj = _store.root().object(elem);
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            _clearUpdated(it, getData);
        }
    }
}

template <typename GetDataFn>
void MinMax::_setTypeObject(MinMaxStore::Obj& obj, GetDataFn getData) {
    auto prev = getData(obj.element()).type();
    if (prev != MinMaxStore::Type::kObject) {
        getData(obj.element()).setObject();
        for (auto& subelem : obj) {
            getData(subelem).setUnset();
        }
    }
}

template <typename GetDataFn>
void MinMax::_setTypeArray(MinMaxStore::Obj& obj, GetDataFn getData) {
    auto prev = getData(obj.element()).type();
    if (prev != MinMaxStore::Type::kArray) {
        getData(obj.element()).setArray();
        for (auto& subelem : obj) {
            getData(subelem).setUnset();
        }
    }
}

}  // namespace mongo::timeseries
