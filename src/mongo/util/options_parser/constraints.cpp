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

#include "mongo/util/options_parser/constraints.h"

#include <pcrecpp.h>

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
            sb << "Error: Attempting to set " << _key << " to value: " << intVal
               << " which is out of range: (" << _min << "," << _max << ")";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    } else {
        StringBuilder sb;
        sb << "Error: " << _key << " is of type: " << val.typeToString()
           << " but must be of a numeric type.";
        return Status(ErrorCodes::BadValue, sb.str());
    }
    return Status::OK();
}

Status ImmutableKeyConstraint::check(const Environment& env) {
    Value env_value;
    Status ret = env.get(_key, &env_value);
    if (ret.isOK()) {
        if (_value.isEmpty()) {
            _value = env_value;
        } else {
            if (!_value.equal(env_value)) {
                StringBuilder sb;
                sb << "Error: " << _key << " is immutable once set";
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
    }

    return ret;
}

Status MutuallyExclusiveKeyConstraint::check(const Environment& env) {
    Value env_value;
    Status ret = env.get(_key, &env_value);
    if (ret.isOK()) {
        ret = env.get(_otherKey, &env_value);
        if (ret.isOK()) {
            StringBuilder sb;
            sb << _otherKey << " is not allowed when " << _key << " is specified";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    return Status::OK();
}

Status RequiresOtherKeyConstraint::check(const Environment& env) {
    Value env_value;
    Status ret = env.get(_key, &env_value);
    if (ret.isOK()) {
        ret = env.get(_otherKey, &env_value);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << _otherKey << " is required when " << _key << " is specified";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    return Status::OK();
}

Status StringFormatKeyConstraint::check(const Environment& env) {
    Value value;
    Status ret = env.get(_key, &value);
    if (ret.isOK()) {
        std::string stringVal;
        ret = value.get(&stringVal);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << _key << " could not be read as a string: " << ret.reason();
            return Status(ErrorCodes::BadValue, sb.str());
        }

        pcrecpp::RE re(_regexFormat);
        if (!re.FullMatch(stringVal)) {
            StringBuilder sb;
            sb << _key << " must be a string of the format: " << _displayFormat;
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    return Status::OK();
}

}  // namespace optionenvironment
}  // namespace mongo
