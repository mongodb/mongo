// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
 *
 * Deprecated: IDL should now be preferred for imposing structure onto BSON.
 */

template <typename T>
class [[MONGO_MOD_USE_REPLACEMENT(idl)]] BSONFieldValue {
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
class [[MONGO_MOD_USE_REPLACEMENT(idl)]] BSONField {
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
