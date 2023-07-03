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

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <string>

#include "mongo/bson/bsonobj.h"

namespace mongo {

/**
 * A BSONField holds the name and the type intended for a given BSON element. The
 * class helps documenting and enforcing that field's type.
 *
 * Example usages:
 *
 *     In a header file:
 *     // Determines the types for the fields used in a collection.
 *     static const std::string MyColl;
 *     struct MyCollFields {
 *         static BSONField<std::string> name;
 *         static BSONField<bool> draining;
 *         static BSONField<int> count;
 *     };
 *
 *     In a cpp file:
 *     const std::string MyColl = "my_collection_name";
 *
 *     // determines the names used for the fields
 *     BSONField<std::string> MyCollFields::name("_id");
 *     BSONField<bool> MyCollFields::draining("draining");
 *     BSONField<int> MyCollFields::count("count");
 *
 *     // Use BSONField::operator()(const T&) to instantiate a typed element for the field:
 *     BSONObj obj = BSON(MyCollFields::name("id_for_this_doc") <<
 *                        MyCollFields::draining(true) <<
 *                        MyCollFields::count(0));
 *
 *     // Use BSONField::gt() and friends to construct a basic query predicate over the field:
 *     BSONObj obj = BSON(MyCollFields::count.gt(10));
 *
 *     // Use BSONField::operator()() to retrieve the name of the field:
 *     BSONObj obj = BSON(MyCollFields::draining() << 1);
 */

template <typename T>
class BSONFieldValue {
public:
    BSONFieldValue(const std::string& name, const T& t) : _name(name), _t(t) {}

    const T& value() const {
        return _t;
    }
    const std::string& name() const {
        return _name;
    }

private:
    std::string _name;
    T _t;
};

template <typename T>
class BSONField {
public:
    BSONField(const std::string& name) : _name(name) {}

    BSONField(const std::string& name, const T& defaultVal) : _name(name), _default(defaultVal) {}

    BSONFieldValue<T> make(const T& t) const {
        return BSONFieldValue<T>(_name, t);
    }

    BSONFieldValue<T> operator()(const T& t) const {
        return BSONFieldValue<T>(_name, t);
    }

    const std::string& name() const {
        return _name;
    }

    const T& getDefault() const {
        return *_default;
    }

    bool hasDefault() const {
        return bool(_default);
    }

    std::string operator()() const {
        return _name;
    }

    BSONFieldValue<BSONObj> query(const char* q, const T& t) const;

    BSONFieldValue<BSONObj> gt(const T& t) const {
        return query("$gt", t);
    }

    BSONFieldValue<BSONObj> lt(const T& t) const {
        return query("$lt", t);
    }

    BSONFieldValue<BSONObj> ne(const T& t) const {
        return query("$ne", t);
    }

private:
    std::string _name;
    boost::optional<T> _default;
};

}  // namespace mongo
