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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/options_parser/environment.h"

namespace mongo {
namespace optionenvironment {

    /** A Constraint validates an Environment.  It has one function, which takes an Environment as
     *  an argument and returns either a success or failure Status depending on whether the
     *  Environment was valid according to this constraint
     *
     *  These are meant to be registered with an Environment to define what it means for that
     *  Environment to be "valid" and run as part of its validation process
     */
    class Constraint {
    public:
        // Interface
        Status operator()(const Environment& env) { return check(env); }
        virtual ~Constraint() {}
    private:
        // Implementation
        virtual Status check(const Environment&) = 0;
    };

    /** A KeyConstraint is a Constraint on a specific Key.  Currently this is not handled any
     *  differently than a Constraint, and is only here as a way to help document whether a
     *  Constraint applies to a single Key or an Environment as a whole
     */
    class KeyConstraint : public Constraint {
    public:
        KeyConstraint(const Key& key) :
            _key(key)
    { }
        virtual ~KeyConstraint() {}
    protected:
        Key _key;
    };

    /** Implementation of a Constraint on the range of a numeric Value.  Fails if the Value is not a
     *  number, or if it is a number but outside the given range
     */
    class NumericKeyConstraint : public KeyConstraint {
    public:
        NumericKeyConstraint(const Key& k, long min, long max) :
            KeyConstraint(k),
            _min(min),
            _max(max)
    { }
        virtual ~NumericKeyConstraint() {}

    private:
        virtual Status check(const Environment& env);
        long _min;
        long _max;
    };

    /** Implementation of a Constraint that makes a Value immutable.  Fails if the Value has already
     *  been set and we are attempting to set it to a different Value.  Note that setting it to the
     *  same value is allowed in this implementation
     */
    class ImmutableKeyConstraint : public KeyConstraint {
    public:
        ImmutableKeyConstraint(const Key& k) : KeyConstraint(k)
    { }
        virtual ~ImmutableKeyConstraint() {}

    private:
        virtual Status check(const Environment& env);
        Value _value;
    };

    /** Implementation of a Constraint on the type of a Value.  Fails if we cannot extract the given
     *  type from our Value, which means the implementation of the access functions of Value
     *  controls which types are "compatible"
     */
    template <typename T>
    class TypeKeyConstraint : public KeyConstraint {
    public:
        TypeKeyConstraint(const Key& k) :
            KeyConstraint(k)
    { }
        virtual ~TypeKeyConstraint() {}

    private:
        virtual Status check(const Environment& env) {
            Value val;
            Status s = env.get(_key, &val);
            if (!s.isOK()) {
                // Key not set, skipping type constraint check
                return Status::OK();
            }

            // The code that controls whether a type is "compatible" is contained in the Value
            // class, so if that handles compatibility between numeric types then this will too.
            T typedVal;
            if (!val.get(&typedVal).isOK()) {
                StringBuilder sb;
                sb << "Error: value for key: " << _key << " was found as type: "
                << val.typeToString() << " but is required to be type: " << typeid(typedVal).name();
                return Status(ErrorCodes::InternalError, sb.str());
            }
            return Status::OK();
        }
    };

} // namespace optionenvironment
} // namespace mongo
