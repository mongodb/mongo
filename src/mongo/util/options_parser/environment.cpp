/* Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/util/options_parser/environment.h"

#include <iostream>

#include "mongo/bson/util/builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/options_parser/constraints.h"

namespace mongo {
namespace optionenvironment {

using std::shared_ptr;
using std::string;
using std::type_info;

// Environment implementation

Status Environment::addKeyConstraint(KeyConstraint* keyConstraint) {
    keyConstraints.push_back(keyConstraint);
    return Status::OK();
}
Status Environment::addConstraint(Constraint* constraint) {
    constraints.push_back(constraint);
    return Status::OK();
}

/** Get the value at Key.  Note that we should not be able to add empty values to the
 *  environment, so we don't check for that here */
Status Environment::get(const Key& get_key, Value* get_value) const {
    typedef std::map<Key, Value>::const_iterator it_type;
    it_type value = values.find(get_key);
    if (value == values.end()) {
        value = default_values.find(get_key);
        if (value == default_values.end()) {
            StringBuilder sb;
            sb << "Value not found for key: " << get_key;
            return Status(ErrorCodes::NoSuchKey, sb.str());
        }
    }
    *get_value = value->second;
    return Status::OK();
}

/** Set the Value in our Environment.  Always disallow empty values */
Status Environment::set(const Key& add_key, const Value& add_value) {
    // 1. Make sure value is not empty
    if (add_value.isEmpty()) {
        return Status(ErrorCodes::InternalError, "Attempted to add an empty value");
    }

    // 2. Save old values
    std::map<Key, Value> old_values = values;

    // 3. Add value to be added
    values[add_key] = add_value;

    // 4. Validate only if our environment is already valid
    if (valid) {
        Status ret = validate();
        if (!ret.isOK()) {
            // 5. Revert to old values if this was invalid
            values = old_values;
            return ret;
        }
    }

    return Status::OK();
}

/** Removes a Value from our Environment */
Status Environment::remove(const Key& remove_key) {
    // 1. Save old values
    std::map<Key, Value> old_values = values;

    // 2. Remove value to be removed
    values.erase(remove_key);

    // 3. Validate only if our environment is already valid
    if (valid) {
        Status ret = validate();
        if (!ret.isOK()) {
            // 4. Revert to old values if this was invalid
            values = old_values;
            return ret;
        }
    }

    return Status::OK();
}

/** Set the default Value for the given Key in our Environment.  Always disallow empty values */
Status Environment::setDefault(const Key& add_key, const Value& add_value) {
    // 1. Make sure value is not empty
    if (add_value.isEmpty()) {
        return Status(ErrorCodes::InternalError, "Attempted to set an empty default value");
    }

    // 2. Disallow modifying defaults after calling validate on this Environment
    if (valid) {
        return Status(ErrorCodes::InternalError,
                      "Attempted to set a default value after calling validate");
    }

    // 3. Add this value to our defaults
    default_values[add_key] = add_value;

    return Status::OK();
}

/** Set all the Values from the source Environment in our Environment.  Does not check for empty
 *  values as the source Environment should not have been allowed to have any */
Status Environment::setAll(const Environment& add_environment) {
    // 1. Save old values
    std::map<Key, Value> old_values = values;

    // 2. Add values to be added
    std::map<Key, Value> add_values = add_environment.values;
    for (std::map<Key, Value>::const_iterator iterator = add_values.begin();
         iterator != add_values.end();
         iterator++) {
        values[iterator->first] = iterator->second;
    }

    // 3. Validate only if our environment is already valid
    if (valid) {
        Status ret = validate();
        if (!ret.isOK()) {
            // 4. Revert to old values if this was invalid
            values = old_values;
            return ret;
        }
    }

    return Status::OK();
}

/** Validate the Environment by iterating over all our constraints and calling them on our
 *  Environment
 */
Status Environment::validate(bool setValid) {
    // 1. Iterate and check all KeyConstraints
    typedef std::vector<KeyConstraint*>::iterator it_keyConstraint;
    for (it_keyConstraint iterator = keyConstraints.begin(); iterator != keyConstraints.end();
         iterator++) {
        Status ret = (**iterator)(*this);
        if (!ret.isOK()) {
            return ret;
        }
    }

    // 2. Iterate and check all Constraints
    typedef std::vector<Constraint*>::iterator it_constraint;
    for (it_constraint iterator = constraints.begin(); iterator != constraints.end(); iterator++) {
        Status ret = (**iterator)(*this);
        if (!ret.isOK()) {
            return ret;
        }
    }

    // 3. Our Environment is now valid.  Record this if we should and return success
    if (setValid) {
        valid = true;
    }
    return Status::OK();
}

/** Implementation of legacy interface to be consistent with
 *  boost::program_options::variables_map during the transition period
 *
 *  boost::program_options::variables_map inherits the count function from std::map, which
 *  returns 1 if the value is set, and 0 if it is not set
 */
bool Environment::count(const Key& key) const {
    Value value;
    Status ret = get(key, &value);
    if (ret.isOK()) {
        return true;
    } else {
        return false;
    }
}

Value Environment::operator[](const Key& key) const {
    Value value;
    Status ret = get(key, &value);
    return value;
}

/* Debugging */
void Environment::dump() const {
    std::map<Key, Value>::const_iterator iter;
    for (iter = values.begin(); iter != values.end(); ++iter) {
        std::cout << "Key: '" << iter->first << "', Value: '" << iter->second.toString() << "'"
                  << std::endl;
    }
}

namespace {

// Converts a map of values with dotted key names to a BSONObj with sub objects.
// 1. Check for dotted field names and call valueMapToBSON recursively.
// 2. Append the actual value to our builder if we did not find a dot in our key name.
Status valueMapToBSON(const std::map<Key, Value>& params,
                      BSONObjBuilder* builder,
                      const std::string& prefix = std::string()) {
    for (std::map<Key, Value>::const_iterator it(params.begin()); it != params.end(); it++) {
        Key key = it->first;
        Value value = it->second;

        // 1. Check for dotted field names and call valueMapToBSON recursively.
        // NOTE: this code depends on the fact that std::map is sorted
        //
        // EXAMPLE:
        // The map:
        // {
        //     "var1.dotted1" : false,
        //     "var2" : true,
        //     "var1.dotted2" : 6
        // }
        //
        // Gets sorted by keys as:
        // {
        //     "var1.dotted1" : false,
        //     "var1.dotted2" : 6,
        //     "var2" : true
        // }
        //
        // Which means when we see the "var1" prefix, we can iterate until we see either a name
        // without a dot or without "var1" as a prefix, aggregating its fields in a new map as
        // we go.  Because the map is sorted, once we see a name without a dot or a "var1"
        // prefix we know that we've seen everything with "var1" as a prefix and can recursively
        // build the entire sub object at once using our new map (which is the only way to make
        // a single coherent BSON sub object using this append only builder).
        //
        // The result of this function for this example should be a BSON object of the form:
        // {
        //     "var1" : {
        //         "dotted1" : false,
        //         "dotted2" : 6
        //     },
        //     "var2" : true
        // }

        // Check to see if this key name is dotted
        std::string::size_type dotOffset = key.find('.');
        if (dotOffset != string::npos) {
            // Get the name of the "section" that we are currently iterating.  This will be
            // the name of our sub object.
            std::string sectionName = key.substr(0, dotOffset);

            // Build a map of the "section" that we are iterating to be passed in a
            // recursive call.
            std::map<Key, Value> sectionMap;

            std::string beforeDot = key.substr(0, dotOffset);
            std::string afterDot = key.substr(dotOffset + 1, key.size() - dotOffset - 1);
            std::map<Key, Value>::const_iterator it_next = it;

            do {
                // Here we know that the key at it_next has a dot and has the prefix we are
                // currently creating a sub object for.  Since that means we will definitely
                // process that element in this loop, advance the outer for loop iterator here.
                it = it_next;

                // Add the value to our section map with a key of whatever is after the dot
                // since the section name itself will be part of our sub object builder.
                sectionMap[afterDot] = value;

                // Peek at the next value for our iterator and check to see if we've finished.
                if (++it_next == params.end()) {
                    break;
                }
                key = it_next->first;
                value = it_next->second;

                // Look for a dot for our next iteration.
                dotOffset = key.find('.');

                beforeDot = key.substr(0, dotOffset);
                afterDot = key.substr(dotOffset + 1, key.size() - dotOffset - 1);
            } while (dotOffset != string::npos && beforeDot == sectionName);

            // Use the section name in our object builder, and recursively call
            // valueMapToBSON with our sub map with keys that have the section name removed.
            BSONObjBuilder sectionObjBuilder(builder->subobjStart(sectionName));
            valueMapToBSON(sectionMap, &sectionObjBuilder, sectionName);
            sectionObjBuilder.done();

            // Our iterator is currently on the last field that matched our dot and prefix, so
            // continue to the next loop iteration.
            continue;
        }

        // 2. Append the actual value to our builder if we did not find a dot in our key name.
        const type_info& type = value.type();

        if (type == typeid(string)) {
            if (value.as<string>().empty()) {
                // boost po uses empty string for flags like --quiet
                // TODO: Remove this when we remove boost::program_options
                builder->appendBool(key, true);
            } else {
                builder->append(key, value.as<string>());
            }
        } else if (type == typeid(int))
            builder->append(key, value.as<int>());
        else if (type == typeid(double))
            builder->append(key, value.as<double>());
        else if (type == typeid(bool))
            builder->appendBool(key, value.as<bool>());
        else if (type == typeid(long))
            builder->appendNumber(key, (long long)value.as<long>());
        else if (type == typeid(unsigned))
            builder->appendNumber(key, (long long)value.as<unsigned>());
        else if (type == typeid(unsigned long long))
            builder->appendNumber(key, (long long)value.as<unsigned long long>());
        else if (type == typeid(StringVector_t))
            builder->append(key, value.as<StringVector_t>());
        else if (type == typeid(StringMap_t)) {
            BSONObjBuilder subBuilder(builder->subobjStart(key));
            StringMap_t stringMap = value.as<StringMap_t>();
            for (StringMap_t::iterator stringMapIt = stringMap.begin();
                 stringMapIt != stringMap.end();
                 stringMapIt++) {
                subBuilder.append(stringMapIt->first, stringMapIt->second);
            }
            subBuilder.done();
        } else
            builder->append(key, "UNKNOWN TYPE: " + demangleName(type));
    }
    return Status::OK();
}
}  // namespace

BSONObj Environment::toBSON() const {
    BSONObjBuilder builder;
    Status ret = valueMapToBSON(values, &builder);
    if (!ret.isOK()) {
        return BSONObj();
    }
    return builder.obj();
}

}  // namespace optionenvironment
}  // namespace mongo
