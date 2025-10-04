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

#include "mongo/base/data_range.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <memory>

typedef struct _bson_value_t bson_value_t;

namespace mongo {

/**
 * C++ friendly wrapper around libbson's bson_value_t and its associated functions.
 */
class LibBSONValue {
public:
    /**
     * Initialized as a EOO type.
     */
    LibBSONValue();
    ~LibBSONValue();

    /**
     * Copy the value from  its source.
     */
    LibBSONValue(const BSONElement& elem);
    LibBSONValue(const bson_value_t& value);

    LibBSONValue(const LibBSONValue& src) : LibBSONValue(*(src._value.get())) {}
    LibBSONValue& operator=(const LibBSONValue& src);
    LibBSONValue& operator=(LibBSONValue&&);

    LibBSONValue(LibBSONValue&&) = default;

    /**
     * Parse a string of bytes containing an encoded BSON value.
     */
    LibBSONValue(BSONType bsonType, ConstDataRange cdr);

    /**
     * Write the stored value into a BSONObj.
     */
    void serialize(StringData fieldName, BSONObjBuilder* builder) const;

    /**
     * Write the stored value into a BSONArray.
     */
    void serialize(BSONArrayBuilder* builder) const;

    /**
     * Basic accessor.
     */
    const bson_value_t* get() const {
        return _value.get();
    }

    /**
     * Serialize the value into an object with no fieldName.
     * Callers can use getObject().firstElement() to use it as a BSONElement.
     */
    BSONObj getObject() const;

private:
    std::unique_ptr<bson_value_t> _value;
};

}  // namespace mongo
