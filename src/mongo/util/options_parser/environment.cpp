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
#include "mongo/util/options_parser/constraints.h"

namespace mongo {
namespace optionenvironment {

    // Environment implementation

    Status Environment::addKeyConstraint(KeyConstraint* c) {
        keyConstraints.push_back(boost::shared_ptr<KeyConstraint>(c));
        return Status::OK();
    }
    Status Environment::addConstraint(Constraint* c) {
        constraints.push_back(boost::shared_ptr<Constraint>(c));
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
        for(std::map<Key, Value>::const_iterator iterator = add_values.begin();
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
        typedef std::vector<boost::shared_ptr<KeyConstraint> >::iterator it_keyConstraint;
        for(it_keyConstraint iterator = keyConstraints.begin();
            iterator != keyConstraints.end(); iterator++) {
            Status ret = (*(*iterator).get())(*this);
            if (!ret.isOK()) {
                return ret;
            }
        }

        // 2. Iterate and check all Constraints
        typedef std::vector<boost::shared_ptr<Constraint> >::iterator it_constraint;
        for(it_constraint iterator = constraints.begin();
            iterator != constraints.end(); iterator++) {
            Status ret = (*(*iterator).get())(*this);
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

} // namespace optionenvironment
} // namespace mongo
