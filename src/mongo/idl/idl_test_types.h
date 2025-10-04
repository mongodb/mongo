/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <vector>

namespace mongo {

/**
 * Simple class that demonstrates the contract a class must implement to parse an IDL "any" type.
 */
class AnyBasicType {
public:
    static AnyBasicType parseFromBSON(const BSONElement& element) {
        AnyBasicType any;
        any._element = element;
        return any;
    }

    /**
     * Serialize this class as a field in a document.
     */
    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
        builder->appendAs(_element, fieldName);
    }

    /**
     * Serialize this class as an element of a BSON array.
     */
    void serializeToBSON(BSONArrayBuilder* builder) const {
        builder->append(_element);
    }

private:
    BSONElement _element;
};

/**
 * Simple class that demonstrates the contract a class must implement to parse a BSON "object" type
 * from the IDL parser.
 */
class ObjectBasicType {
public:
    static ObjectBasicType parseFromBSON(const BSONObj& obj) {
        ObjectBasicType object;
        object._obj = obj.getOwned();
        return object;
    }

    BSONObj serializeToBSON() const {
        return _obj;
    }

private:
    BSONObj _obj;
};

/**
 * Simple class that demonstrates the contract a class must implement to parse a BSON "bindata"
 * variable length type
 * from the IDL parser.
 */
class BinDataCustomType {
public:
    BinDataCustomType() {}
    BinDataCustomType(std::vector<std::uint8_t>& vec) : _vec(std::move(vec)) {}

    static BinDataCustomType parseFromBSON(const std::vector<std::uint8_t> vec) {
        BinDataCustomType b;
        b._vec = std::move(vec);
        return b;
    }

    ConstDataRange serializeToBSON() const {
        return ConstDataRange(_vec);
    }

    const std::vector<std::uint8_t>& getVector() const {
        return _vec;
    }

private:
    std::vector<std::uint8_t> _vec;
};

/**
 * Simple class that demonstrates the contract a class must implement to parse an IDL  "chain" type
 * from the IDL parser.
 */
class ChainedType {
public:
    static ChainedType parseFromBSON(const BSONObj& obj) {
        ChainedType object;
        object._str = obj["field1"].str();
        return object;
    }

    void serializeToBSON(BSONObjBuilder* builder) const {
        builder->append("field1", _str);
    }

    StringData getField1() const {
        return _str;
    }
    void setField1(StringData value) {
        _str = std::string{value};
    }

private:
    std::string _str;
};

class AnotherChainedType {
public:
    static AnotherChainedType parseFromBSON(const BSONObj& obj) {
        AnotherChainedType object;
        object._num = obj["field2"].numberLong();
        return object;
    }

    void serializeToBSON(BSONObjBuilder* builder) const {
        builder->append("field2", _num);
    }

    std::int64_t getField2() const {
        return _num;
    }
    void setField2(std::int64_t value) {
        _num = value;
    }

private:
    std::int64_t _num;
};

class ViewChainedType {
public:
    static ViewChainedType parseFromBSON(const BSONObj& obj) {
        ViewChainedType object;
        object._obj = obj;
        return object;
    }

    void serializeToBSON(BSONObjBuilder* builder) const {
        builder->append("view_type", _obj);
    }

    BSONObj getView_type() const {
        return _obj;
    }
    void setView_type(BSONObj obj) {
        _obj = obj;
    }

private:
    BSONObj _obj;
};

}  // namespace mongo
