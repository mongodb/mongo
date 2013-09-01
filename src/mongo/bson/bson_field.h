/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

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
     *     static const string MyColl;
     *     struct MyCollFields {
     *         static BSONField<string> name;
     *         static BSONField<bool> draining;
     *         static BSONField<int> count;
     *     };
     *
     *     In a cpp file:
     *     const string MyColl = "my_collection_name";
     *
     *     // determines the names used for the fields
     *     BSONField<string> MyCollFields::name("_id");
     *     BSONField<bool> MyCollFields::draining("draining");
     *     BSONField<int> MyCollFields::count("count");
     *
     *     In an insert:
     *     conn->insert(myColl,
     *                  BSON(MyCollFields::name("id_for_this_doc") <<
     *                       MyCollFields::draining(true) <<
     *                       MyCollFields::count(0)));
     *
     *     In a query:
     *     conn->findOne(myColl, BSON(MyCollFields::count.gt(10))) ;
     *
     *     In a command:
     *     conn->ensureIndex(mycoll, BSON(MyCollFields::draining() << 1), true);
     */

    template<typename T>
    class BSONFieldValue {
    public:
        BSONFieldValue(const std::string& name, const T& t)
            : _name(name), _t(t) { }

        const T& value() const { return _t; }
        const std::string& name() const { return _name; }

    private:
        std::string _name;
        T _t;
    };

    template<typename T>
    class BSONField {
    public:
        BSONField(const std::string& name)
            : _name(name), _defaultSet(false) {}

        BSONField(const std::string& name, const T& defaultVal)
            : _name(name), _default(defaultVal) , _defaultSet(true) {}

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
            dassert(_defaultSet);
            return _default;
        }

        const bool hasDefault() const {
            return _defaultSet;
        }

        std::string operator()() const {
            return _name;
        }

        BSONFieldValue<BSONObj> query(const char * q, const T& t) const;

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
        T _default;
        bool _defaultSet;
    };

} // namespace mongo
