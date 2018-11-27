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
/* The contents of this file are meant to be used by
 * code generated from idlc.py.
 *
 * It should not be instantiated directly from mongo code,
 * rather parameters should be defined in .idl files.
 */

#include <functional>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {
namespace idl_server_parameter_detail {

template <typename T>
inline StatusWith<T> coerceFromString(StringData str) {
    T value;
    Status status = parseNumberFromString(str, &value);
    if (!status.isOK()) {
        return status;
    }
    return value;
}

template <>
inline StatusWith<bool> coerceFromString<bool>(StringData str) {
    if ((str == "1") || (str == "true")) {
        return true;
    }
    if ((str == "0") || (str == "false")) {
        return false;
    }
    return {ErrorCodes::BadValue, "Value is not a valid boolean"};
}

template <>
inline StatusWith<std::string> coerceFromString<std::string>(StringData str) {
    return str.toString();
}

template <>
inline StatusWith<std::vector<std::string>> coerceFromString<std::vector<std::string>>(
    StringData str) {
    std::vector<std::string> v;
    splitStringDelim(str.toString(), &v, ',');
    return v;
}

// Predicate rules for bounds conditions.
struct GT {
    static constexpr StringData description = "greater than"_sd;
    template <typename T, typename U>
    static constexpr bool evaluate(const T& a, const U& b) {
        return a > b;
    }
};

struct LT {
    static constexpr StringData description = "less than"_sd;
    template <typename T, typename U>
    static constexpr bool evaluate(const T& a, const U& b) {
        return a < b;
    }
};

struct GTE {
    static constexpr StringData description = "greater than or equal to"_sd;
    template <typename T, typename U>
    static constexpr bool evaluate(const T& a, const U& b) {
        return a >= b;
    }
};

struct LTE {
    static constexpr StringData description = "less than or equal to"_sd;
    template <typename T, typename U>
    static constexpr bool evaluate(const T& a, const U& b) {
        return a <= b;
    }
};

// Wrapped type unwrappers.
// e.g. Given AtomicInt32, get std::int32_t and normalized store/load methods.
template <typename U>
struct storage_wrapper;

template <typename U>
struct storage_wrapper<AtomicWord<U>> {
    static constexpr bool thread_safe = true;
    using type = U;
    static void store(AtomicWord<U>& storage, const U& value) {
        storage.store(value);
    }
    static U load(const AtomicWord<U>& storage) {
        return storage.load();
    }
};

// Covers AtomicDouble
template <typename U, typename P>
struct storage_wrapper<AtomicProxy<U, P>> {
    static constexpr bool thread_safe = true;
    using type = U;
    static void store(AtomicProxy<U, P>& storage, const U& value) {
        storage.store(value);
    }
    static U load(const AtomicProxy<U, P>& storage) {
        return storage.load();
    }
};

template <typename U>
struct storage_wrapper<synchronized_value<U>> {
    static constexpr bool thread_safe = true;
    using type = U;
    static void store(synchronized_value<U>& storage, const U& value) {
        *storage = value;
    }
    static U load(const synchronized_value<U>& storage) {
        return *storage;
    }
};

// All other types
template <typename U>
struct storage_wrapper {
    static constexpr bool thread_safe = false;
    using type = U;
    static void store(U& storage, const U& value) {
        storage = value;
    }
    static U load(const U& storage) {
        return storage;
    }
};

}  // namespace idl_server_parameter_detail

/**
 * Specialization of ServerParameter used by IDL generator.
 */
template <typename T>
class IDLServerParameterWithStorage : public ServerParameter {
private:
    using SPT = ServerParameterType;
    using SW = idl_server_parameter_detail::storage_wrapper<T>;

public:
    static constexpr bool thread_safe = SW::thread_safe;
    using element_type = typename SW::type;

    IDLServerParameterWithStorage(StringData name, T& storage, ServerParameterType paramType)
        : ServerParameter(ServerParameterSet::getGlobal(),
                          name,
                          paramType == SPT::kStartupOnly || paramType == SPT::kStartupAndRuntime,
                          paramType == SPT::kRuntimeOnly || paramType == SPT::kStartupAndRuntime),
          _storage(storage) {
        invariant(thread_safe || paramType == SPT::kStartupOnly,
                  "Runtime server parameters must be thread safe");
    }

    /**
     * Convenience wrapper for storing a value.
     */
    Status setValue(const element_type& newValue) {
        for (const auto& validator : _validators) {
            const auto status = validator(newValue);
            if (!status.isOK()) {
                return status;
            }
        }
        SW::store(_storage, newValue);

        if (_onUpdate) {
            return _onUpdate(newValue);
        }

        return Status::OK();
    }

    /**
     * Convenience wrapper for fetching value from storage.
     */
    element_type getValue() const {
        return SW::load(_storage);
    }

    /**
     * Encode the setting into BSON object.
     *
     * Typically invoked by {getParameter:...} to produce a dictionary
     * of SCP settings.
     */
    void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) final {
        b.append(name, getValue());
    }

    /**
     * Update the underlying value using a BSONElement
     *
     * Allows setting non-basic values (e.g. vector<string>)
     * via the {setParameter: ...} call.
     */
    Status set(const BSONElement& newValueElement) final {
        element_type newValue;

        if (!newValueElement.coerce(&newValue)) {
            return Status(ErrorCodes::BadValue, "Can't coerce value");
        }

        return setValue(newValue);
    }

    /**
     * Update the underlying value from a string.
     *
     * Typically invoked from commandline --setParameter usage.
     */
    Status setFromString(const std::string& str) final {
        auto swNewValue = idl_server_parameter_detail::coerceFromString<element_type>(str);
        if (!swNewValue.isOK()) {
            return swNewValue.getStatus();
        }

        return setValue(swNewValue.getValue());
    }

    /**
     * Called *after* updating the underlying storage to its new value.
     */
    using onUpdate_t = Status(const element_type&);
    void setOnUpdate(std::function<onUpdate_t> onUpdate) {
        _onUpdate = std::move(onUpdate);
    }

    // Validators.

    /**
     * Add a callback validator to be invoked when this setting is updated.
     *
     * Callback should return Status::OK() or ErrorCodes::BadValue.
     */
    using validator_t = Status(const element_type&);
    void addValidator(std::function<validator_t> validator) {
        _validators.push_back(std::move(validator));
    }

    /**
     * Sets a validation limit against a predicate function.
     */
    template <class predicate>
    void addBound(const element_type& bound) {
        addValidator([ bound, spname = name() ](const element_type& value) {
            if (!predicate::evaluate(value, bound)) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Invalid value for parameter " << spname << ": "
                                            << value
                                            << " is not "
                                            << predicate::description
                                            << " "
                                            << bound);
            }
            return Status::OK();
        });
    }

private:
    T& _storage;

    std::vector<std::function<validator_t>> _validators;
    std::function<onUpdate_t> _onUpdate;
};

// MSVC has trouble resolving T=decltype(param) through the above class template.
// Avoid that by using this proxy factory to infer storage type.
template <typename T>
IDLServerParameterWithStorage<T>* makeIDLServerParameterWithStorage(StringData name,
                                                                    T& storage,
                                                                    ServerParameterType spt) {
    return new IDLServerParameterWithStorage<T>(name, storage, spt);
}

}  // namespace mongo
