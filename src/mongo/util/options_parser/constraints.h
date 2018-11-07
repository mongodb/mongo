
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
    Status operator()(const Environment& env) {
        return check(env);
    }
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
    KeyConstraint(const Key& key) : _key(key) {}
    virtual ~KeyConstraint() {}

protected:
    Key _key;
};

/** Implementation of a Constraint on the range of a numeric Value.  Fails if the Value is not a
 *  number, or if it is a number but outside the given range
 */
class NumericKeyConstraint : public KeyConstraint {
public:
    NumericKeyConstraint(const Key& k, long min, long max)
        : KeyConstraint(k), _min(min), _max(max) {}
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
    ImmutableKeyConstraint(const Key& k) : KeyConstraint(k) {}
    virtual ~ImmutableKeyConstraint() {}

private:
    virtual Status check(const Environment& env);
    Value _value;
};

/** Implementation of a Constraint that makes two keys mutually exclusive.  Fails if both keys
 *  are set.
 */
class MutuallyExclusiveKeyConstraint : public KeyConstraint {
public:
    MutuallyExclusiveKeyConstraint(const Key& key, const Key& otherKey)
        : KeyConstraint(key), _otherKey(otherKey) {}
    virtual ~MutuallyExclusiveKeyConstraint() {}

private:
    virtual Status check(const Environment& env);
    Key _otherKey;
};

/** Implementation of a Constraint that makes one key require another.  Fails if the first key
 *  is set but the other is not.
 */
class RequiresOtherKeyConstraint : public KeyConstraint {
public:
    RequiresOtherKeyConstraint(const Key& key, const Key& otherKey)
        : KeyConstraint(key), _otherKey(otherKey) {}
    virtual ~RequiresOtherKeyConstraint() {}

private:
    virtual Status check(const Environment& env);
    Key _otherKey;
};

/** Implementation of a Constraint that enforces a specific format on a std::string value.  Fails if
 *  the value of the key is not a std::string or does not match the given regex.
 */
class StringFormatKeyConstraint : public KeyConstraint {
public:
    StringFormatKeyConstraint(const Key& key,
                              const std::string& regexFormat,
                              const std::string& displayFormat)
        : KeyConstraint(key), _regexFormat(regexFormat), _displayFormat(displayFormat) {}
    virtual ~StringFormatKeyConstraint() {}

private:
    virtual Status check(const Environment& env);
    std::string _regexFormat;
    std::string _displayFormat;
};

/** Implementation of a Constraint on the type of a Value.  Fails if we cannot extract the given
 *  type from our Value, which means the implementation of the access functions of Value
 *  controls which types are "compatible"
 */
template <typename T>
class TypeKeyConstraint : public KeyConstraint {
public:
    TypeKeyConstraint(const Key& k) : KeyConstraint(k) {}
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
            sb << "Error: value for key: " << _key << " was found as type: " << val.typeToString()
               << " but is required to be type: " << typeid(typedVal).name();
            return Status(ErrorCodes::InternalError, sb.str());
        }
        return Status::OK();
    }
};

/**
 * Proxy constraint for callbacks used by IDL based config options with a key.
 * Callback may take either the entire environment, or just the value being validated.
 */
template <typename T>
class CallbackKeyConstraint : public KeyConstraint {
public:
    using Callback = std::function<Status(const Environment&, const Key&)>;
    using ValueCallback = std::function<Status(const T&)>;

    CallbackKeyConstraint(const Key& k, ValueCallback callback)
        : KeyConstraint(k), _valueCallback(std::move(callback)) {}
    CallbackKeyConstraint(const Key& k, Callback callback)
        : KeyConstraint(k), _callback(std::move(callback)) {}

private:
    Status check(const Environment& env) override {
        if (_callback) {
            return _callback(env, _key);
        }

        if (!_valueCallback) {
            return Status::OK();
        }

        Value val;
        auto status = env.get(_key, &val);
        if (!status.isOK()) {
            // Key not set, skipping callback constraint check.
            return Status::OK();
        }

        T typedVal;
        if (!val.get(&typedVal).isOK()) {
            return {ErrorCodes::InternalError,
                    str::stream() << "Error: value for key: " << _key << " was found as type: "
                                  << val.typeToString()
                                  << " but is required to be type: "
                                  << typeid(typedVal).name()};
        }

        return _valueCallback(typedVal);
    }

    Callback _callback;
    ValueCallback _valueCallback;
};

/**
 * General boundary constraint for numeric type values.
 */
template <typename T>
class BoundaryKeyConstraint : public CallbackKeyConstraint<T> {
public:
    BoundaryKeyConstraint(const Key& k,
                          const boost::optional<T>& gt,
                          const boost::optional<T>& lt,
                          const boost::optional<T>& gte,
                          const boost::optional<T>& lte)
        : CallbackKeyConstraint<T>(k, [=](const T& val) -> Status {
              if (gt && !(val > *gt)) {
                  return {ErrorCodes::BadValue,
                          str::stream() << k << " must be greater than " << *gt};
              }
              if (lt && !(val < *lt)) {
                  return {ErrorCodes::BadValue, str::stream() << k << " must be less than " << *lt};
              }
              if (gte && !(val >= *gte)) {
                  return {ErrorCodes::BadValue,
                          str::stream() << k << " must be greater than or equal to " << *gte};
              }
              if (lte && !(val <= *lte)) {
                  return {ErrorCodes::BadValue,
                          str::stream() << k << " must be less than or equal to " << *lte};
              }
              return Status::OK();
          }) {}
};

}  // namespace optionenvironment
}  // namespace mongo
