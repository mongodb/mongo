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

#include "mongo/util/options_parser/constraints.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"

namespace mongo {
namespace optionenvironment {

    Status NumericKeyConstraint::check(const Environment& env) {
        Value val;
        Status s = env.get(_key, &val);

        if (s == ErrorCodes::NoSuchKey) {
            // Key not set, skipping numeric constraint check
            return Status::OK();
        }

        // The code that controls whether a type is "compatible" is contained in the Value
        // class, so if that handles compatibility between numeric types then this will too.
        long intVal;
        if (val.get(&intVal).isOK()) {
            if (intVal < _min || intVal > _max) {
                StringBuilder sb;
                sb << "Error: Attempting to set " << _key << " to value: " <<
                    intVal << " which is out of range: (" <<
                    _min << "," << _max << ")";
                return Status(ErrorCodes::InternalError, sb.str());
            }
        }
        else {
            StringBuilder sb;
            sb << "Error: " << _key << " is of type: " << val.typeToString() <<
                " but must be of a numeric type.";
            return Status(ErrorCodes::InternalError, sb.str());
        }
        return Status::OK();
    }

    Status ImmutableKeyConstraint::check(const Environment& env) {
        Value env_value;
        Status ret = env.get(_key, &env_value);
        if (ret.isOK()) {
            if (_value.isEmpty()) {
                _value = env_value;
            }
            else {
                if (!_value.equal(env_value)) {
                    StringBuilder sb;
                    sb << "Error: " << _key << " is immutable once set";
                    return Status(ErrorCodes::InternalError, sb.str());
                }
            }
        }

        return ret;
    }

} // namespace optionenvironment
} // namespace mongo
