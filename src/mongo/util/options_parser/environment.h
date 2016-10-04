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

#pragma once

#include <map>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/options_parser/value.h"

namespace mongo {
namespace optionenvironment {

class Constraint;
class KeyConstraint;

typedef std::string Key;

/** An Environment is a map of values that can be validated according to a set of registered
 *  constraints
 *
 *  Usage overview:
 *
 *  1. Create an empty Environment
 *  2. Add Constraints
 *  3. Set Key/Value pairs (will not cause constraints to be triggered)
 *  4. Validate (will run all constraints)
 *  5. Access
 *  6. Set/Modify Key/Value pairs (will run all constraints and reject invalid modifications)
 *  7. Access
 *
 *  Since the constraints are run whenever we try to set or modify Key/Value pairs after we
 *  validate, we have the invariant that the Environment is always valid according to its
 *  Constraints after validation.  Adding new constraints is disallowed after validation.
 *
 *  Usage example:
 *
 *  // Create an empty Environment
 *  Environment environment;
 *
 *  // Initialize our first Key and Value
 *  Key key1("key1");
 *  Value value1(1);
 *
 *  // Add a Constraint on "key1"
 *  Status ret = environment.addConstraint(new ImmutableKeyConstraint("key1"));
 *  if (!ret.isOK()) {
 *      return ret;
 *  }
 *
 *  // Set our first Key and Value to the Environment
 *  ret = environment.set(key1, value1);
 *  if (!ret.isOK()) {
 *      return ret;
 *  }
 *
 *  // Attempt to mutate should be successful, since validate has not been called
 *  ret = environment.set(key1, Value(2));
 *  if (!ret.isOK()) {
 *      return ret;
 *  }
 *
 *  // Validate our Environment
 *  ret = environment.validate();
 *  if (!ret.isOK()) {
 *      return ret;
 *  }
 *
 *  // Access our Environment
 *  int intvalue1;
 *  ret = environment.get(key1, &intvalue1);
 *  if (!ret.isOK()) {
 *      return ret;
 *  }
 *
 *  // Attempt to mutate should fail, since validate has been called
 *  ret = environment.set(key1, Value(3));
 *  if (!ret.isOK()) {
 *      return ret;
 *  }
 */

class Environment {
public:
    Environment() : valid(false) {}
    ~Environment() {}

    /** These functions are to add Constraints and KeyConstraints which will be run against
     *  this environment in the following situations:
     *  1. in the "validate" function
     *  2. in the "set" function after validate has been called successfully
     *
     *  It is an error to call these functions after "validate" has been called
     *
     *  NOTE: These DO NOT take ownership of the pointer passed in
     */
    Status addKeyConstraint(KeyConstraint* keyConstraint);
    Status addConstraint(Constraint* constraint);

    /** Add the Value to this Environment with the given Key.  If "validate" has already
     *  been called on this Environment, runs all Constraints on the new Environment.  If
     *  any of the Constraints fail, reverts to the old Environment and returns an error
     */
    Status set(const Key& key, const Value& value);

    /** Remove the Value from this Environment with the given Key.  If "validate" has
     * already been called on this Environment, runs all Constraints on the new Environment.
     * If any of the Constraints fail, reverts to the old Environment and returns an error
     */
    Status remove(const Key& key);

    /** Add a default Value to this Environment with the given Key.  Fails if validate has
     *  already been called on our environment.  The get functions will return the default
     *  if one exists and the value has not been explicitly set.
     */
    Status setDefault(const Key& key, const Value& value);

    /** Populate the given Value with the Value stored for the given Key.  Return a success
     *  status if the value was found, or an error status if the value was not found.
     *  Leaves the Value unchanged on error.
     */
    Status get(const Key& key, Value* value) const;

    /** Same as the above get interface, but supports directly getting C++ types without the
     *  intermediate Value and has the added failure case of the value being the wrong type
     */
    template <typename T>
    Status get(const Key& key, T* value_contents) const;

    /** Runs all registered Constraints and returns the result.  If "setValid" is true and
     * validation succeeds, marks this as a valid Environment so that any modifications will
     * re run all Constraints
     */
    Status validate(bool setValid = true);

    /** Sets all variables in the given Environment in this Environment.  Does not add
     *  Constraints
     */
    Status setAll(const Environment& other);

    /** The functions below are the legacy interface to be consistent with
     *  boost::program_options::variables_map during the transition period
     */

    /**
     *  @return 1 if the given Key has a Value set in this Environment and 0 if not
     */
    bool count(const Key& key) const;

    /**
     *  @return the Value for the given Key in this Environment.  Returns an empty Value if
     *  Key is not set.
     */
    Value operator[](const Key& key) const;

    /**
     * Gets the BSON representation of this Environment.  This will collapse dotted fields
     * into sub objects.
     *
     * Example:
     *
     * The following Environment values map:
     *  "a.b.c" -> true
     *  "a.b.d" -> false
     *  "a.e.f" -> 0
     *  "a.e.g" -> 1
     *  "a.h" -> "foo"
     *
     * Has a BSON represation of (shown as JSON):
     *  { "a" : {
     *           "b" : {
     *                  "c" : true,
     *                  "d" : false
     *                 },
     *           "e" : {
     *                  "f" : 0,
     *                  "g" : 1
     *                 },
     *           "h" : "foo"
     *          }
     *  }
     *
     * Note that the BSON representation only includes fields that were explicitly set using
     * setAll or set, and not defaults that were specified using setDefault.
     */
    BSONObj toBSON() const;

    /* Debugging */
    void dump() const;

protected:
    std::vector<Constraint*> constraints;
    std::vector<KeyConstraint*> keyConstraints;
    std::map<Key, Value> values;
    std::map<Key, Value> default_values;
    bool valid;
};

template <typename T>
Status Environment::get(const Key& get_key, T* get_value) const {
    Value value;
    Status ret = get(get_key, &value);
    if (!ret.isOK()) {
        return ret;
    }
    ret = value.get(get_value);
    if (!ret.isOK()) {
        StringBuilder sb;
        sb << "Error getting value for key: \"" << get_key << "\": " << ret.toString();
        return Status(ErrorCodes::NoSuchKey, sb.str());
    }
    return Status::OK();
}

}  // namespace optionenvironment
}  // namespace mongo
