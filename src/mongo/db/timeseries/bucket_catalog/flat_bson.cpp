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

#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstring>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/str.h"

namespace mongo::timeseries::bucket_catalog {
namespace {
constexpr int32_t kMaxLinearSearchLength = 12;
constexpr StringData kArrayFieldName =
    "\0"_sd;  // Use a string that is illegal to represent fields in BSON

int typeComp(const BSONElement& elem, BSONType type) {
    return elem.canonicalType() - canonicalizeBSONType(type);
};
}  // namespace

template <class Element, class Value>
FlatBSONStore<Element, Value>::Data::Data(TrackingContext& trackingContext)
    : _value(trackingContext) {}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Type FlatBSONStore<Element, Value>::Data::type() const {
    return _type;
}

template <class Element, class Value>
const Value& FlatBSONStore<Element, Value>::Data::value() const {
    return _value;
}

/**
 * Flag to indicate if this Data was updated since last clear.
 */
template <class Element, class Value>
bool FlatBSONStore<Element, Value>::Data::updated() const {
    return _updated;
}

/**
 * Clear update flag.
 */
template <class Element, class Value>
void FlatBSONStore<Element, Value>::Data::clearUpdated() {
    _updated = false;
}


template <class Element, class Value>
void FlatBSONStore<Element, Value>::Data::setUnset() {
    _type = Type::kUnset;
    _updated = false;
}

template <class Element, class Value>
void FlatBSONStore<Element, Value>::Data::setObject() {
    _type = Type::kObject;
    _updated = true;
}

template <class Element, class Value>
void FlatBSONStore<Element, Value>::Data::setArray() {
    _type = Type::kArray;
    _updated = true;
}
template <class Element, class Value>
void FlatBSONStore<Element, Value>::Data::setValue(const BSONElement& elem) {
    _value.set(elem);
    _type = Type::kValue;
    _updated = true;
}

template <class Element, class Value>
FlatBSONStore<Element, Value>::Entry::Entry(TrackingContext& trackingContext)
    : _element(trackingContext) {}

template <class Element, class Value>
FlatBSONStore<Element, Value>::Iterator::Iterator(
    typename FlatBSONStore<Element, Value>::Entries::iterator pos)
    : _pos(pos) {}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Iterator::pointer
FlatBSONStore<Element, Value>::Iterator::operator->() {
    return &_pos->_element;
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Iterator::reference
FlatBSONStore<Element, Value>::Iterator::operator*() {
    return _pos->_element;
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Iterator&
FlatBSONStore<Element, Value>::Iterator::operator++() {
    _pos += _pos->_offsetEnd;
    return *this;
}

template <class Element, class Value>
bool FlatBSONStore<Element, Value>::Iterator::operator==(
    const FlatBSONStore<Element, Value>::Iterator& rhs) const {
    return _pos == rhs._pos;
}

template <class Element, class Value>
bool FlatBSONStore<Element, Value>::Iterator::operator!=(
    const FlatBSONStore<Element, Value>::Iterator& rhs) const {
    return !operator==(rhs);
}

template <class Element, class Value>
FlatBSONStore<Element, Value>::ConstIterator::ConstIterator(
    typename FlatBSONStore<Element, Value>::Entries::const_iterator pos)
    : _pos(pos) {}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::ConstIterator::pointer
FlatBSONStore<Element, Value>::ConstIterator::operator->() const {
    return &_pos->_element;
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::ConstIterator::reference
FlatBSONStore<Element, Value>::ConstIterator::operator*() const {
    return _pos->_element;
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::ConstIterator&
FlatBSONStore<Element, Value>::ConstIterator::operator++() {
    _pos += _pos->_offsetEnd;
    return *this;
}

template <class Element, class Value>
bool FlatBSONStore<Element, Value>::ConstIterator::operator==(const ConstIterator& rhs) const {
    return _pos == rhs._pos;
}

template <class Element, class Value>
bool FlatBSONStore<Element, Value>::ConstIterator::operator!=(
    const FlatBSONStore<Element, Value>::ConstIterator& rhs) const {
    return !operator==(rhs);
}

template <class Element, class Value>
FlatBSONStore<Element, Value>::Obj::Obj(
    TrackingContext& trackingContext,
    FlatBSONStore<Element, Value>::Entries& entries,
    typename FlatBSONStore<Element, Value>::Entries::iterator pos)
    : _entries(entries), _pos(pos), _trackingContext(trackingContext) {}


template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Obj& FlatBSONStore<Element, Value>::Obj::operator=(
    const FlatBSONStore::Obj& rhs) {
    if (this != &rhs) {
        _pos = rhs._pos;
    }
    return *this;
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Obj FlatBSONStore<Element, Value>::Obj::object(
    FlatBSONStore<Element, Value>::Iterator pos) const {
    return {_trackingContext, _entries, pos._pos};
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Obj FlatBSONStore<Element, Value>::Obj::parent() const {
    return {_trackingContext, _entries, _pos - _pos->_offsetParent};
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Iterator FlatBSONStore<Element, Value>::Obj::iterator()
    const {
    return {_pos};
}

template <class Element, class Value>
Element& FlatBSONStore<Element, Value>::Obj::element() {
    return _pos->_element;
}

template <class Element, class Value>
const Element& FlatBSONStore<Element, Value>::Obj::element() const {
    return _pos->_element;
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Iterator FlatBSONStore<Element, Value>::Obj::search(
    FlatBSONStore<Element, Value>::Iterator first,
    FlatBSONStore<Element, Value>::Iterator last,
    StringData fieldName) {
    // Use fast lookup if available.
    if (_pos->_fieldNameToIndex) {
        auto it = _pos->_fieldNameToIndex->find(fieldName);
        if (it == _pos->_fieldNameToIndex->end()) {
            return last;
        }

        return {_pos + it->second};
    }

    // Perform linear search forward.
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

    // Return if we reached our end when doing linear search.
    if (first == last) {
        return last;
    }

    // We've exhausted the linear search limit, create a map to speedup future searches. Populate it
    // with all current subelements.
    _pos->_fieldNameToIndex = makeTrackedStringMap<uint32_t>(_trackingContext);
    auto it = begin();
    auto itEnd = end();
    for (; it != itEnd; ++it) {
        _pos->_fieldNameToIndex->try_emplace(
            make_tracked_string(_trackingContext, it->fieldName().data(), it->fieldName().size()),
            it._pos->_offsetParent);
    }

    // Retry the search now when the map is created.
    return search(first, last, fieldName);
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Iterator FlatBSONStore<Element, Value>::Obj::search(
    FlatBSONStore<Element, Value>::Iterator first, StringData fieldName) {
    return search(first, end(), fieldName);
}

template <class Element, class Value>
std::pair<typename FlatBSONStore<Element, Value>::Iterator,
          typename FlatBSONStore<Element, Value>::Iterator>
FlatBSONStore<Element, Value>::Obj::insert(FlatBSONStore<Element, Value>::Iterator pos,
                                           std::string fieldName) {
    // Remember our iterator position so we can restore it after inserting a new element.
    auto index = std::distance(_entries.begin(), _pos);
    auto inserted = _entries.emplace(pos._pos, _trackingContext);
    _pos = _entries.begin() + index;

    // Setup our newly created entry.
    inserted->_offsetEnd = 1;  // no subelements
    inserted->_element.setFieldName(std::move(fieldName));
    inserted->_offsetParent = std::distance(_pos, inserted);

    // Also store our offset in the fast lookup map if it is available.
    if (_pos->_fieldNameToIndex) {
        _pos->_fieldNameToIndex->try_emplace(
            make_tracked_string(_trackingContext,
                                inserted->_element.fieldName().data(),
                                inserted->_element.fieldName().size()),
            inserted->_offsetParent);
    }

    // We need to traverse the hiearchy up to the root and modify stored offsets to account for
    // this newly created entry. All entries with subobjects got their end-offset pushed by one.
    // All siblings after this entry got their offset to the parent pushed by one.
    auto it = inserted;
    auto parent = _pos;

    // Root object has "self" as parent.
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

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Iterator FlatBSONStore<Element, Value>::Obj::begin() {
    return {_pos + 1};
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::Iterator FlatBSONStore<Element, Value>::Obj::end() {
    return {_pos + _pos->_offsetEnd};
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::ConstIterator FlatBSONStore<Element, Value>::Obj::begin()
    const {
    return {_pos + 1};
}

template <class Element, class Value>
typename FlatBSONStore<Element, Value>::ConstIterator FlatBSONStore<Element, Value>::Obj::end()
    const {
    return {_pos + _pos->_offsetEnd};
}

template <class Element, class Value>
FlatBSONStore<Element, Value>::FlatBSONStore(TrackingContext& trackingContext)
    : entries(make_tracked_vector<Entry>(trackingContext)), _trackingContext(trackingContext) {
    auto& entry = entries.emplace_back(trackingContext);
    entry._offsetEnd = 1;
    entry._offsetParent = 0;
    entry._element.initializeRoot();
}

template <class Derived, class Element, class Value>
FlatBSON<Derived, Element, Value>::FlatBSON(TrackingContext& trackingContext)
    : _store(trackingContext), _trackingContext(trackingContext) {}

template <class Derived, class Element, class Value>
typename std::string FlatBSON<Derived, Element, Value>::updateStatusString(
    UpdateStatus updateStatus) {
    switch (updateStatus) {
        case UpdateStatus::Updated:
            return "updated";
        case UpdateStatus::Failed:
            return "failed";
        case UpdateStatus::NoChange:
            return "no change";
    }

    MONGO_UNREACHABLE;
}

template <class Derived, class Element, class Value>
typename FlatBSON<Derived, Element, Value>::UpdateStatus FlatBSON<Derived, Element, Value>::update(
    const BSONObj& doc,
    boost::optional<StringData> omitField,
    const StringDataComparator* stringComparator) {
    auto obj = _store.root();
    return _updateObj(obj, doc, {}, stringComparator, [&omitField](StringData fieldName) {
        return omitField && fieldName == omitField;
    });
}

template <class Derived, class Element, class Value>
std::tuple<typename FlatBSON<Derived, Element, Value>::UpdateStatus,
           typename FlatBSONStore<Element, Value>::Iterator,
           typename FlatBSONStore<Element, Value>::Iterator>
FlatBSON<Derived, Element, Value>::_update(typename FlatBSONStore<Element, Value>::Obj obj,
                                           BSONElement elem,
                                           typename Element::UpdateContext updateValues,
                                           const StringDataComparator* stringComparator) {
    UpdateStatus status{UpdateStatus::Updated};

    if (elem.type() == Object) {
        std::tie(status, updateValues) = Derived::_shouldUpdateObj(obj, elem, updateValues);
        // Compare objects element-wise if the stored data may need to be updated.
        if (status == UpdateStatus::Updated) {
            status = _updateObj(
                obj, elem.Obj(), updateValues, stringComparator, [](StringData fieldName) {
                    return false;
                });
        }
    } else if (elem.type() == Array) {
        std::tie(status, updateValues) = Derived::_shouldUpdateArr(obj, elem, updateValues);
        // Compare objects element-wise if the stored data may need to be updated.
        if (status == UpdateStatus::Updated) {
            // Use Obj() instead of Array() to avoid instantiating a temporary std::vector.
            auto elemArray = elem.Obj();
            auto elemIt = elemArray.begin();
            auto elemEnd = elemArray.end();

            auto it = obj.begin();
            auto end = obj.end();
            for (; elemIt != elemEnd && status != UpdateStatus::Failed; ++elemIt) {
                UpdateStatus subStatus{UpdateStatus::NoChange};

                if (it == end) {
                    std::tie(it, end) = obj.insert(it, kArrayFieldName.toString());
                }

                std::tie(subStatus, it, end) =
                    _update(obj.object(it), *elemIt, updateValues, stringComparator);
                if (subStatus != UpdateStatus::NoChange) {
                    status = subStatus;
                }

                obj = obj.object(it).parent();
                ++it;
            }
        }
    } else {
        // For all scalar types, just update the value directly if needed.
        status = Derived::_maybeUpdateValue(obj, elem, updateValues, stringComparator);
    }

    return {status, obj.iterator(), obj.parent().end()};
}

template <class Derived, class Element, class Value>
typename FlatBSON<Derived, Element, Value>::UpdateStatus
FlatBSON<Derived, Element, Value>::_updateObj(typename FlatBSONStore<Element, Value>::Obj& obj,
                                              const BSONObj& doc,
                                              typename Element::UpdateContext updateContext,
                                              const StringDataComparator* stringComparator,
                                              std::function<bool(StringData)> skipFieldFn) {
    auto it = obj.begin();
    auto end = obj.end();
    int allHandledOffset = 0;
    bool skipped = false;

    UpdateStatus status{UpdateStatus::NoChange};

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
            // Traversing the FlatBSONStore structure in lock-step with the input document resulted
            // in a miss. This means one of two things. (1) The input document does not contain all
            // prevously-encountered fields and we need to skip over missing ones. Or (2) the input
            // document has a different internal field order than previous inserts. We begin by
            // searching forward to see if we are case (1).
            auto found = obj.search(std::next(it), fieldName);
            if (found == end) {
                // Field not found. We can either be in case (2) or this is a new field that needs
                // to be inserted.
                if (skipped) {
                    // Search over the skipped elements in case we are actually in case (2).
                    auto begin = obj.begin();
                    std::advance(begin, allHandledOffset);
                    found = obj.search(begin, it, fieldName);
                    if (found == it) {
                        // Still not found, insert the new field. Location doesn't matter much
                        // as we are operating on incoming documents of different field orders.
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
        UpdateStatus subStatus{UpdateStatus::NoChange};
        std::tie(subStatus, it, end) =
            _update(obj.object(it), elem, updateContext, stringComparator);
        if (subStatus != UpdateStatus::NoChange) {
            status = subStatus;
        }

        // Re-construct obj from the returned iterator. If an insert was performed inside
        // _update it would dangle.
        obj = obj.object(it).parent();

        // Advance iterator and advance the all handled offset if we have not skipped anything.
        ++it;
        if (!skipped) {
            ++allHandledOffset;
        }

        if (status == UpdateStatus::Failed) {
            // We can fail early to save some time iterating over the rest of the structure.
            break;
        }
    }

    return status;
}

template <class Derived, class Element, class Value>
template <typename GetDataFn>
void FlatBSON<Derived, Element, Value>::_append(typename FlatBSONStore<Element, Value>::Obj obj,
                                                BSONObjBuilder* builder,
                                                GetDataFn getData) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const auto& data = getData(*it);
        if (data.type() == FlatBSONStore<Element, Value>::Type::kValue) {
            builder->appendAs(data.value().get(), it->fieldName());
        } else if (data.type() == FlatBSONStore<Element, Value>::Type::kObject) {
            BSONObjBuilder subObj(builder->subobjStart(it->fieldName()));
            _append(obj.object(it), &subObj, getData);
        } else if (data.type() == FlatBSONStore<Element, Value>::Type::kArray) {
            BSONArrayBuilder subArr(builder->subarrayStart(it->fieldName()));
            _append(obj.object(it), &subArr, getData);
        }
        if (data.updated())
            _clearUpdated(it, getData);
    }
}

template <class Derived, class Element, class Value>
template <typename GetDataFn>
void FlatBSON<Derived, Element, Value>::_append(typename FlatBSONStore<Element, Value>::Obj obj,
                                                BSONArrayBuilder* builder,
                                                GetDataFn getData) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const auto& data = getData(*it);
        if (data.type() == FlatBSONStore<Element, Value>::Type::kValue) {
            builder->append(data.value().get());
        } else if (data.type() == FlatBSONStore<Element, Value>::Type::kObject) {
            BSONObjBuilder subObj(builder->subobjStart());
            _append(obj.object(it), &subObj, getData);
        } else if (data.type() == FlatBSONStore<Element, Value>::Type::kArray) {
            BSONArrayBuilder subArr(builder->subarrayStart());
            _append(obj.object(it), &subArr, getData);
        }
        if (data.updated())
            _clearUpdated(it, getData);
    }
}

template <class Derived, class Element, class Value>
template <typename GetDataFn>
bool FlatBSON<Derived, Element, Value>::_appendUpdates(
    typename FlatBSONStore<Element, Value>::Obj obj, BSONObjBuilder* builder, GetDataFn getData) {
    const typename FlatBSONStore<Element, Value>::Data& data = getData(obj.element());
    invariant((data.type() == FlatBSONStore<Element, Value>::Type::kObject ||
               data.type() == FlatBSONStore<Element, Value>::Type::kArray));

    bool appended = false;
    if (data.type() == FlatBSONStore<Element, Value>::Type::kObject) {
        bool hasUpdateSection = false;
        BSONObjBuilder updateSection;
        StringMap<BSONObj> subDiffs;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const auto& subdata = getData(*it);
            if (subdata.updated()) {
                if (subdata.type() == FlatBSONStore<Element, Value>::Type::kObject) {
                    BSONObjBuilder subObj(updateSection.subobjStart(it->fieldName()));
                    _append(obj.object(it), &subObj, getData);
                } else if (subdata.type() == FlatBSONStore<Element, Value>::Type::kArray) {
                    BSONArrayBuilder subArr(updateSection.subarrayStart(it->fieldName()));
                    _append(obj.object(it), &subArr, getData);
                } else {
                    updateSection.appendAs(subdata.value().get(), it->fieldName());
                }
                _clearUpdated(it, getData);
                appended = true;
                hasUpdateSection = true;
            } else if (subdata.type() != FlatBSONStore<Element, Value>::Type::kValue &&
                       subdata.type() != FlatBSONStore<Element, Value>::Type::kUnset) {
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
                if (subdata.type() == FlatBSONStore<Element, Value>::Type::kObject) {
                    BSONObjBuilder subObj(builder->subobjStart(updateFieldName));
                    _append(obj.object(it), &subObj, getData);
                } else if (subdata.type() == FlatBSONStore<Element, Value>::Type::kArray) {
                    BSONArrayBuilder subArr(builder->subarrayStart(updateFieldName));
                    _append(obj.object(it), &subArr, getData);
                } else {
                    builder->appendAs(subdata.value().get(), updateFieldName);
                }
                _clearUpdated(it, getData);
                appended = true;
            } else if (subdata.type() != FlatBSONStore<Element, Value>::Type::kValue &&
                       subdata.type() != FlatBSONStore<Element, Value>::Type::kUnset) {
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

template <class Derived, class Element, class Value>
template <typename GetDataFn>
void FlatBSON<Derived, Element, Value>::_clearUpdated(
    typename FlatBSONStore<Element, Value>::Iterator elem, GetDataFn getData) {
    auto& data = getData(*elem);

    data.clearUpdated();
    if (data.type() == FlatBSONStore<Element, Value>::Type::kObject ||
        data.type() == FlatBSONStore<Element, Value>::Type::kArray) {
        auto obj = _store.root().object(elem);
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            _clearUpdated(it, getData);
        }
    }
}

template <class Derived, class Element, class Value>
template <typename GetDataFn>
void FlatBSON<Derived, Element, Value>::_setTypeObject(
    typename FlatBSONStore<Element, Value>::Obj& obj, GetDataFn getData) {
    auto prev = getData(obj.element()).type();
    if (prev != FlatBSONStore<Element, Value>::Type::kObject) {
        getData(obj.element()).setObject();
        for (auto& subelem : obj) {
            getData(subelem).setUnset();
        }
    }
}

template <class Derived, class Element, class Value>
template <typename GetDataFn>
void FlatBSON<Derived, Element, Value>::_setTypeArray(
    typename FlatBSONStore<Element, Value>::Obj& obj, GetDataFn getData) {
    auto prev = getData(obj.element()).type();
    if (prev != FlatBSONStore<Element, Value>::Type::kArray) {
        getData(obj.element()).setArray();
        for (auto& subelem : obj) {
            getData(subelem).setUnset();
        }
    }
}

BSONElementValueBuffer::BSONElementValueBuffer(TrackingContext& trackingContext)
    : _buffer(make_tracked_vector<char>(trackingContext)) {}

BSONElement BSONElementValueBuffer::get() const {
    return BSONElement(_buffer.data(), 1, _size);
}

void BSONElementValueBuffer::set(const BSONElement& elem) {
    _size = elem.size() - elem.fieldNameSize() + 1;
    if (_buffer.size() < _size) {
        _buffer.resize(_size);
    }
    auto buffer = _buffer.data();
    // Store element as BSONElement buffer but strip out the field name.
    buffer[0] = elem.type();
    buffer[1] = '\0';
    memcpy(buffer + 2, elem.value(), elem.valuesize());
}

BSONType BSONElementValueBuffer::type() const {
    return (BSONType)_buffer[0];
}

size_t BSONElementValueBuffer::size() const {
    return _size;
}

BSONElement BSONTypeValue::get() const {
    MONGO_UNREACHABLE;
}

void BSONTypeValue::set(const BSONElement& elem) {
    _type = elem.type();
}

BSONType BSONTypeValue::type() const {
    return _type;
}

int64_t BSONTypeValue::size() const {
    return 0;
}

Element::Element(TrackingContext& trackingContext)
    : _fieldName(make_tracked_string(trackingContext)) {}

StringData Element::fieldName() const {
    return {_fieldName.data(), _fieldName.size()};
}

void Element::setFieldName(std::string&& fieldName) {
    _fieldName = std::move(fieldName);
}

bool Element::isArrayFieldName() const {
    return fieldName() == kArrayFieldName;
}

void Element::claimArrayFieldNameForObject(std::string name) {
    invariant(isArrayFieldName());
    _fieldName = std::move(name);
}

MinMaxElement::MinMaxElement(TrackingContext& trackingContext)
    : Element(trackingContext), _min(trackingContext), _max(trackingContext) {}

void MinMaxElement::initializeRoot() {
    _min.setObject();
    _max.setObject();
}

MinMaxStore::Data& MinMaxElement::min() {
    return _min;
}

const MinMaxStore::Data& MinMaxElement::min() const {
    return _min;
}

MinMaxStore::Data& MinMaxElement::max() {
    return _max;
}

const MinMaxStore::Data& MinMaxElement::max() const {
    return _max;
}

MinMax::MinMax(TrackingContext& trackingContext)
    : FlatBSON<MinMax, MinMaxElement, BSONElementValueBuffer>(trackingContext) {}

std::pair<MinMax::UpdateStatus, MinMaxElement::UpdateContext> MinMax::_shouldUpdateObj(
    MinMaxStore::Obj& obj, const BSONElement& elem, MinMaxElement::UpdateContext updateValues) {

    auto shouldUpdateObject = [&](MinMaxStore::Data& data, auto comp) {
        return data.type() == MinMaxStore::Type::kObject ||
            data.type() == MinMaxStore::Type::kUnset ||
            (data.type() == MinMaxStore::Type::kArray && comp(typeComp(elem, Array), 0)) ||
            (data.type() == MinMaxStore::Type::kValue &&
             comp(typeComp(elem, data.value().type()), 0));
    };

    bool updateMin = updateValues.min && shouldUpdateObject(obj.element().min(), std::less<int>{});
    if (updateMin) {
        _setTypeObject(obj, GetMin{});
    }

    bool updateMax =
        updateValues.max && shouldUpdateObject(obj.element().max(), std::greater<int>{});
    if (updateMax) {
        _setTypeObject(obj, GetMax{});
    }

    return std::make_pair((updateMin || updateMax) ? UpdateStatus::Updated : UpdateStatus::NoChange,
                          MinMaxElement::UpdateContext{updateMin, updateMax});
}

std::pair<MinMax::UpdateStatus, MinMaxElement::UpdateContext> MinMax::_shouldUpdateArr(
    MinMaxStore::Obj& obj, const BSONElement& elem, MinMaxElement::UpdateContext updateValues) {
    auto shouldUpdateArray = [&](MinMaxStore::Data& data, auto comp) {
        return data.type() == MinMaxStore::Type::kArray ||
            data.type() == MinMaxStore::Type::kUnset ||
            (data.type() == MinMaxStore::Type::kObject && comp(typeComp(elem, Object), 0)) ||
            (data.type() == MinMaxStore::Type::kValue &&
             comp(typeComp(elem, data.value().type()), 0));
    };

    bool updateMin = updateValues.min && shouldUpdateArray(obj.element().min(), std::less<int>{});
    if (updateMin) {
        _setTypeArray(obj, GetMin{});
    }

    bool updateMax =
        updateValues.max && shouldUpdateArray(obj.element().max(), std::greater<int>{});
    if (updateMax) {
        _setTypeArray(obj, GetMax{});
    }

    return std::make_pair((updateMin || updateMax) ? UpdateStatus::Updated : UpdateStatus::NoChange,
                          MinMaxElement::UpdateContext{updateMin, updateMax});
}

MinMax::UpdateStatus MinMax::_maybeUpdateValue(MinMaxStore::Obj& obj,
                                               const BSONElement& elem,
                                               MinMaxElement::UpdateContext updateValues,
                                               const StringDataComparator* stringComparator) {
    auto maybeUpdateValue = [&](MinMaxStore::Data& data, auto comp) {
        if (data.type() == MinMaxStore::Type::kUnset ||
            (data.type() == MinMaxStore::Type::kObject && comp(typeComp(elem, Object), 0)) ||
            (data.type() == MinMaxStore::Type::kArray && comp(typeComp(elem, Array), 0)) ||
            (data.type() == MinMaxStore::Type::kValue &&
             comp(elem.woCompare(data.value().get(), false, stringComparator), 0))) {
            data.setValue(elem);
        }
    };

    if (updateValues.min) {
        maybeUpdateValue(obj.element().min(), std::less<>{});
    }

    if (updateValues.max) {
        maybeUpdateValue(obj.element().max(), std::greater<>{});
    }

    return (updateValues.min || updateValues.max) ? UpdateStatus::Updated : UpdateStatus::NoChange;
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

MinMax MinMax::parseFromBSON(TrackingContext& trackingContext,
                             const BSONObj& min,
                             const BSONObj& max,
                             const StringDataComparator* stringComparator) {
    MinMax minmax{trackingContext};

    // The metadata field is already excluded from generated min/max summaries.
    UpdateStatus status = minmax.update(min, /*metaField=*/boost::none, stringComparator);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Failed to update min: " << updateStatusString(status),
            status != UpdateStatus::Failed);

    status = minmax.update(max, /*metaField=*/boost::none, stringComparator);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Failed to update max: " << updateStatusString(status),
            status != UpdateStatus::Failed);

    // Clear the updated state as we're only constructing the object from an existing document.
    [[maybe_unused]] auto minUpdates = minmax.minUpdates();
    [[maybe_unused]] auto maxUpdates = minmax.maxUpdates();

    return minmax;
}

SchemaElement::SchemaElement(TrackingContext& trackingContext)
    : Element(trackingContext), _data(trackingContext) {}

void SchemaElement::initializeRoot() {
    _data.setObject();
}

SchemaStore::Data& SchemaElement::data() {
    return _data;
}

const SchemaStore::Data& SchemaElement::data() const {
    return _data;
}

Schema::Schema(TrackingContext& trackingContext)
    : FlatBSON<Schema, SchemaElement, BSONTypeValue>(trackingContext) {}

Schema Schema::parseFromBSON(TrackingContext& trackingContext,
                             const BSONObj& min,
                             const BSONObj& max,
                             const StringDataComparator* stringComparator) {
    Schema schema{trackingContext};

    // The metadata field is already excluded from generated min/max summaries.
    UpdateStatus status = schema.update(min, /*metaField=*/boost::none, stringComparator);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Failed to update min: " << updateStatusString(status),
            status != UpdateStatus::Failed);

    status = schema.update(max, /*metaField=*/boost::none, stringComparator);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Failed to update max: " << updateStatusString(status),
            status != UpdateStatus::Failed);

    return schema;
}

std::pair<Schema::UpdateStatus, SchemaElement::UpdateContext> Schema::_shouldUpdateObj(
    SchemaStore::Obj& obj, const BSONElement& elem, SchemaElement::UpdateContext) {
    UpdateStatus status{UpdateStatus::Updated};
    SchemaStore::Data& data = obj.element().data();

    if (data.type() == SchemaStore::Type::kUnset) {
        _setTypeObject(obj, GetData{});
    } else if (data.type() != SchemaStore::Type::kObject) {
        // Type mismatch
        status = UpdateStatus::Failed;
    }

    return std::make_pair(status, SchemaElement::UpdateContext{});
}

std::pair<Schema::UpdateStatus, SchemaElement::UpdateContext> Schema::_shouldUpdateArr(
    SchemaStore::Obj& obj, const BSONElement& elem, SchemaElement::UpdateContext) {
    UpdateStatus status{UpdateStatus::Updated};
    SchemaStore::Data& data = obj.element().data();

    if (data.type() == SchemaStore::Type::kUnset) {
        _setTypeArray(obj, GetData{});
    } else if (data.type() != SchemaStore::Type::kArray) {
        // Type mismatch
        status = UpdateStatus::Failed;
    }

    return std::make_pair(status, SchemaElement::UpdateContext{});
}

Schema::UpdateStatus Schema::_maybeUpdateValue(SchemaStore::Obj& obj,
                                               const BSONElement& elem,
                                               SchemaElement::UpdateContext,
                                               const StringDataComparator* stringComparator) {
    UpdateStatus status{UpdateStatus::Updated};
    SchemaStore::Data& data = obj.element().data();

    if (data.type() == SchemaStore::Type::kUnset) {
        data.setValue(elem);
    } else if (data.type() != SchemaStore::Type::kValue ||
               typeComp(elem, data.value().type()) != 0) {
        // Type mismatch
        status = UpdateStatus::Failed;
    }

    return status;
}

// Instantiations.
template class FlatBSONStore<MinMaxElement, BSONElementValueBuffer>;
template class FlatBSON<MinMax, MinMaxElement, BSONElementValueBuffer>;

template class FlatBSONStore<SchemaElement, BSONTypeValue>;
template class FlatBSON<Schema, SchemaElement, BSONTypeValue>;

}  // namespace mongo::timeseries::bucket_catalog
