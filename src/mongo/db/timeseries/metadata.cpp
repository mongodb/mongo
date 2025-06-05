/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/timeseries/metadata.h"

#include "mongo/util/tracking/allocator.h"

#include <boost/container/small_vector.hpp>

namespace mongo::timeseries::metadata {
namespace {

template <class Allocator>
void normalizeArray(const BSONObj& obj, allocator_aware::BSONArrayBuilder<Allocator>& builder);
template <class Allocator>
void normalizeObject(const BSONObj& obj, allocator_aware::BSONObjBuilder<Allocator>& builder);

template <class Allocator>
void normalizeArray(const BSONObj& obj, allocator_aware::BSONArrayBuilder<Allocator>& builder) {
    for (auto& arrayElem : obj) {
        if (arrayElem.type() == BSONType::array) {
            allocator_aware::BSONArrayBuilder<Allocator> subArray{builder.subarrayStart()};
            normalizeArray(arrayElem.Obj(), subArray);
        } else if (arrayElem.type() == BSONType::object) {
            allocator_aware::BSONObjBuilder<Allocator> subObject{builder.subobjStart()};
            normalizeObject(arrayElem.Obj(), subObject);
        } else {
            builder.append(arrayElem);
        }
    }
}

template <class Allocator>
void normalizeObject(const BSONObj& obj, allocator_aware::BSONObjBuilder<Allocator>& builder) {
    // BSONObjIteratorSorted provides an abstraction similar to what this function does. However it
    // is using a lexical comparison that is slower than just doing a binary comparison of the field
    // names. That is all we need here as we are looking to create something that is binary
    // comparable no matter of field order provided by the user.

    // Helper that extracts the necessary data from a BSONElement that we can sort and re-construct
    // the same BSONElement from.
    struct Field {
        BSONElement element() const {
            return BSONElement(fieldName.data() - 1,  // Include type byte before field name
                               fieldName.size() + 1,  // Include null terminator after field name
                               BSONElement::TrustedInitTag{});
        }
        bool operator<(const Field& rhs) const {
            return fieldName < rhs.fieldName;
        }
        StringData fieldName;
    };

    // Put all elements in a buffer, sort it and then continue normalize in sorted order
    auto num = obj.nFields();
    static constexpr std::size_t kNumStaticFields = 16;
    boost::container::small_vector<Field, kNumStaticFields> fields;
    fields.resize(num);
    BSONObjIterator bsonIt(obj);
    int i = 0;
    while (bsonIt.more()) {
        auto elem = bsonIt.next();
        fields[i++] = {elem.fieldNameStringData()};
    }
    auto it = fields.begin();
    auto end = fields.end();
    std::sort(it, end);
    for (; it != end; ++it) {
        auto elem = it->element();
        if (elem.type() == BSONType::array) {
            allocator_aware::BSONArrayBuilder<Allocator> subArray(
                builder.subarrayStart(elem.fieldNameStringData()));
            normalizeArray(elem.Obj(), subArray);
        } else if (elem.type() == BSONType::object) {
            allocator_aware::BSONObjBuilder<Allocator> subObject(
                builder.subobjStart(elem.fieldNameStringData()));
            normalizeObject(elem.Obj(), subObject);
        } else {
            builder.append(elem);
        }
    }
}

}  // namespace

template <class Allocator>
void normalize(const BSONElement& elem,
               allocator_aware::BSONObjBuilder<Allocator>& builder,
               boost::optional<StringData> as) {
    if (elem.type() == BSONType::array) {
        allocator_aware::BSONArrayBuilder<Allocator> subArray(
            builder.subarrayStart(as.has_value() ? as.value() : elem.fieldNameStringData()));
        normalizeArray(elem.Obj(), subArray);
    } else if (elem.type() == BSONType::object) {
        allocator_aware::BSONObjBuilder<Allocator> subObject(
            builder.subobjStart(as.has_value() ? as.value() : elem.fieldNameStringData()));
        normalizeObject(elem.Obj(), subObject);
    } else {
        if (as) {
            builder.appendAs(elem, as.value());
        } else {
            builder.append(elem);
        }
    }
}

template void normalize(const BSONElement& elem,
                        allocator_aware::BSONObjBuilder<std::allocator<void>>& builder,
                        boost::optional<StringData> as);
template void normalize(const BSONElement& elem,
                        allocator_aware::BSONObjBuilder<tracking::Allocator<void>>& builder,
                        boost::optional<StringData> as);

}  // namespace mongo::timeseries::metadata
