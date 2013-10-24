/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/util/options_parser/environment.h"

#include <boost/shared_ptr.hpp>
#include <iostream>

#include "mongo/bson/util/builder.h"
#include "mongo/bson/bsonobjiterator.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/options_parser/constraints.h"

namespace mongo {
namespace optionenvironment {

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
        std::map <Key, Value> old_values = values;

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
        std::map <Key, Value> old_values = values;

        // 2. Add values to be added
        std::map <Key, Value> add_values = add_environment.values;
        for (std::map<Key, Value>::const_iterator iterator = add_values.begin();
            iterator != add_values.end(); iterator++) {
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
    Status Environment::validate() {

        // 1. Iterate and check all KeyConstraints
        typedef std::vector<KeyConstraint*>::iterator it_keyConstraint;
        for (it_keyConstraint iterator = keyConstraints.begin();
            iterator != keyConstraints.end(); iterator++) {
            Status ret = (**iterator)(*this);
            if (!ret.isOK()) {
                return ret;
            }
        }

        // 2. Iterate and check all Constraints
        typedef std::vector<Constraint*>::iterator it_constraint;
        for (it_constraint iterator = constraints.begin();
            iterator != constraints.end(); iterator++) {
            Status ret = (**iterator)(*this);
            if (!ret.isOK()) {
                return ret;
            }
        }

        // 3. Our Environment is now valid.  Record this and return success
        valid = true;
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
        }
        else {
            return false;
        }
    }

    Value Environment::operator[](const Key& key) const {
        Value value;
        Status ret = get(key, &value);
        return value;
    }

    /* Debugging */
    void Environment::dump() {
        std::map<Key, Value>::iterator iter;
        for (iter = values.begin(); iter != values.end(); ++iter) {
            std::cout << "Key: '"
                      << iter->first
                      << "', Value: '"
                      << iter->second.toString()
                      << "'" << std::endl;
        }
    }

namespace {

    // Converts a map of values with dotted key names to a BSONObj with sub objects.
    // 1. Check for dotted field names and call valueMapToBSON recursively.
    // 2. Append the actual value to our builder if we did not find a dot in our key name.
    Status valueMapToBSON(const std::map<Key, Value>& params,
                          BSONObjBuilder* builder,
                          const std::string& prefix = std::string()) {
        for (std::map<Key, Value>::const_iterator it(params.begin());
                it != params.end(); it++) {
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
                }
                while (dotOffset != string::npos && beforeDot == sectionName);

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

            if (type == typeid(string)){
                if (value.as<string>().empty()) {
                    // boost po uses empty string for flags like --quiet
                    // TODO: Remove this when we remove boost::program_options
                    builder->appendBool(key, true);
                }
                else {
                    builder->append(key, value.as<string>());
                }
            }
            else if (type == typeid(int))
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
            else if (type == typeid(vector<string>))
                builder->append(key, value.as<vector<string> >());
            else
                builder->append(key, "UNKNOWN TYPE: " + demangleName(type));
        }
        return Status::OK();
    }
} // namespace

    BSONObj Environment::toBSON() const {
        BSONObjBuilder builder;
        Status ret = valueMapToBSON(values, &builder);
        if (!ret.isOK()) {
            return BSONObj();
        }
        return builder.obj();
    }

} // namespace optionenvironment
} // namespace mongo
