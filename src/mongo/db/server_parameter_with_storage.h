// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
/* The contents of this file are meant to be used by
 * code generated from idlc.py.
 *
 * It should not be instantiated directly from mongo code,
 * rather parameters should be defined in .idl files.
 */

#include "mongo/base/error_codes.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/server_parameter.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"
#include "mongo/util/synchronized_value.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>


namespace mongo {
using namespace std::literals::string_view_literals;

namespace [[MONGO_MOD_PUBLIC]] idl_server_parameter_bounds {
// Predicate rules for bounds conditions
struct GT {
    static constexpr inline std::string_view kind = "gt";
    static constexpr inline std::string_view description = "greater than";
    template <typename T, typename U>
    static constexpr bool evaluate(const T& a, const U& b) {
        return a > b;
    }
};

struct LT {
    static constexpr inline std::string_view kind = "lt";
    static constexpr inline std::string_view description = "less than";
    template <typename T, typename U>
    static constexpr bool evaluate(const T& a, const U& b) {
        return a < b;
    }
};

struct GTE {
    static constexpr inline std::string_view kind = "gte";
    static constexpr inline std::string_view description = "greater than or equal to";
    template <typename T, typename U>
    static constexpr bool evaluate(const T& a, const U& b) {
        return a >= b;
    }
};

struct LTE {
    static constexpr inline std::string_view kind = "lte";
    static constexpr inline std::string_view description = "less than or equal to";
    template <typename T, typename U>
    static constexpr bool evaluate(const T& a, const U& b) {
        return a <= b;
    }
};
}  // namespace idl_server_parameter_bounds

namespace idl_server_parameter_detail {

/**
 * Used to check if the parameter type has the getClusterServerParameter method, which proves
 * that ClusterServerParameter is inline chained to it.
 */
template <typename T>
using HasClusterServerParameter = decltype(std::declval<T>().getClusterServerParameter());
template <typename T>
constexpr inline bool hasClusterServerParameter = stdx::is_detected_v<HasClusterServerParameter, T>;

// Wrapped type unwrappers.
// e.g. Given Atomic<int>, get std::int32_t and normalized store/load methods.
template <typename U>
struct storage_wrapper;

template <typename U>
struct storage_wrapper<Atomic<U>> {
    using type = U;
    storage_wrapper(Atomic<U>& storage) : _storage(storage), _defaultValue(storage.load()) {}

    void store(const U& value) {
        _storage.store(value);
    }

    U load() const {
        return _storage.load();
    }

    void reset() {
        _storage.store(_defaultValue);
    }

    // Not thread-safe, will only be called once at most per ServerParameter in its initialization
    // block.
    void setDefault(const U& value) {
        _defaultValue = value;
    }

private:
    Atomic<U>& _storage;

    // Copy of original value to be read from during resets.
    U _defaultValue;
};


template <typename U>
struct storage_wrapper<synchronized_value<U>> {
    using type = U;
    storage_wrapper(synchronized_value<U>& storage) : _storage(storage), _defaultValue(*storage) {}

    void store(const U& value) {
        *_storage = value;
    }

    U load() const {
        return *_storage;
    }

    void reset() {
        *_storage = _defaultValue;
    }

    // Not thread-safe, will only be called once at most per ServerParameter in its initialization
    // block.
    void setDefault(const U& value) {
        _defaultValue = value;
    }

private:
    synchronized_value<U>& _storage;

    // Copy of original value to be read from during resets.
    U _defaultValue;
};

// All other types will use a mutex to synchronize in a threadsafe manner.
template <typename U>
struct storage_wrapper {
    using type = U;
    storage_wrapper(U& storage) : _storage(storage), _defaultValue(storage) {}

    void store(const U& value) {
        std::lock_guard<std::mutex> lg(_storageMutex);
        _storage = value;
    }

    U load() const {
        std::lock_guard<std::mutex> lg(_storageMutex);
        return _storage;
    }

    void reset() {
        std::lock_guard<std::mutex> lg(_storageMutex);
        _storage = _defaultValue;
    }

    // Not thread-safe, will only be called once at most per ServerParameter in its initialization
    // block.
    void setDefault(const U& value) {
        _defaultValue = value;
    }

private:
    mutable std::mutex _storageMutex;
    U& _storage;

    // Copy of original value to be read from during resets.
    U _defaultValue;
};

}  // namespace idl_server_parameter_detail

/**
 * Specialization of ServerParameter used by IDL generator.
 */
template <ServerParameterType paramType, typename T>
class [[MONGO_MOD_PUBLIC]] IDLServerParameterWithStorage : public ServerParameter {
private:
    using SPT = ServerParameterType;
    using SW = idl_server_parameter_detail::storage_wrapper<T>;

public:
    using element_type = typename SW::type;

    // Compile-time assertion to ensure that IDL-defined in-memory storage for CSPs are
    // chained to the ClusterServerParameter base type.
    static_assert((paramType != SPT::kClusterWide) ||
                      idl_server_parameter_detail::hasClusterServerParameter<element_type>,
                  "Cluster server parameter storage must be chained from ClusterServerParameter");

    IDLServerParameterWithStorage(std::string_view name, T& storage)
        : ServerParameter(name, paramType), _storage(storage) {}

    Status validateValue(const element_type& newValue) const {
        for (const auto& validator : _validators) {
            auto status = validator(newValue);
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    /**
     * Convenience wrapper for storing a value.
     */
    Status setValue(const element_type& newValue) {
        // For cluster parameters, validation must be separated from setting.
        if constexpr (paramType != SPT::kClusterWide) {
            if (auto status = validateValue(newValue); !status.isOK()) {
                return status;
            }
        }

        _storage.store(newValue);

        if (_onUpdate) {
            return _onUpdate(newValue);
        }

        return Status::OK();
    }

    /**
     * Convenience wrapper for fetching value from storage.
     */
    element_type getValue() const {
        return _storage.load();
    }

    /**
     * Allows the default value stored in the underlying storage_wrapper to be changed exactly once
     * after initialization. This should only be called by the IDL generator when creating
     * MONGO_SERVER_PARAMETER_REGISTER blocks for parameters that do not specify a `cpp_vartype`
     * (the storage variable is not defined by the IDL generator).
     */
    Status setDefault(const element_type& newDefaultValue) {
        Status status = Status::OK();
        std::call_once(_setDefaultOnce, [&] {
            // Update the default value.
            _storage.setDefault(newDefaultValue);

            // Update the actual storage, performing validation and any post-update functions as
            // necessary.
            status = reset();
        });
        return status;
    }

    /**
     * Encode the setting into BSON object.
     *
     * Typically invoked by {getParameter:...} or {getClusterParameter:...} to produce a dictionary
     * of SCP settings.
     */
    void append(OperationContext* opCtx, BSONObjBuilder* b, std::string_view name) final {
        if (isRedact()) {
            b->append(name, "###");
        } else if constexpr (paramType == SPT::kClusterWide) {
            b->append("_id"sv, name);
            b->appendElementsUnique(getValue().toBSON());
        } else {
            b->append(name, getValue());
        }
    }

    StatusWith<element_type> parseElement(const BSONElement& newValueElement) const {
        element_type newValue;
        if constexpr (paramType == SPT::kClusterWide) {
            try {
                BSONObj cspObj = newValueElement.Obj();
                newValue = element_type::parse(cspObj, IDLParserContext{"ClusterServerParameter"});
            } catch (const DBException& ex) {
                return ex.toStatus().withContext(
                    str::stream() << "Failed parsing ClusterServerParameter '" << name() << "'");
            }
        } else {
            if (auto status = newValueElement.tryCoerce(&newValue); !status.isOK()) {
                return {status.code(),
                        str::stream() << "Failed validating " << name() << ": " << status.reason()};
            }
        }

        return newValue;
    }

    Status validate(const BSONElement& newValueElement) const final {
        StatusWith<element_type> swNewValue = parseElement(newValueElement);
        if (!swNewValue.isOK()) {
            return swNewValue.getStatus();
        }

        return validateValue(swNewValue.getValue());
    }

    /**
     * Update the underlying value using a BSONElement
     *
     * Allows setting non-basic values (e.g. vector<string>)
     * via the {setParameter: ...} call or {setClusterParameter: ...} call.
     */
    Status set(const BSONElement& newValueElement) final {
        StatusWith<element_type> swNewValue = parseElement(newValueElement);
        if (!swNewValue.isOK()) {
            return swNewValue.getStatus();
        }

        return setValue(swNewValue.getValue());
    }

    /**
     * Resets the current storage value in storage_wrapper with the default value.
     */
    Status reset() final {
        _storage.reset();
        if (_onUpdate) {
            return _onUpdate(_storage.load());
        }

        return Status::OK();
    }

    /**
     * Update the underlying value from a string.
     *
     * Typically invoked from commandline --setParameter usage. Prohibited for cluster server
     * parameters.
     */
    Status setFromString(std::string_view str) final {
        if constexpr (paramType == SPT::kClusterWide) {
            return {ErrorCodes::BadValue,
                    "Unable to set a cluster-wide server parameter from the command line or config "
                    "file. See command 'setClusterParameter'"};
        } else {
            auto swNewValue = coerceFromString<element_type>(str);
            if (!swNewValue.isOK()) {
                return swNewValue.getStatus();
            }

            return setValue(swNewValue.getValue());
        }
    }

    /**
     * Retrieves the cluster parameter time from the chained ClusterServerParameter struct in
     * storage. All other server parameters simply return the uninitialized LogicalTime.
     */
    LogicalTime getClusterParameterTime() const final {
        if constexpr (idl_server_parameter_detail::hasClusterServerParameter<element_type>) {
            return getValue().getClusterParameterTime();
        }

        return LogicalTime::kUninitialized;
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
        _bounds.push_back({predicate::kind, bound});
        addValidator([bound, spname = name()](const element_type& value) {
            if (!predicate::evaluate(value, bound)) {
                return Status(ErrorCodes::BadValue,
                              str::stream()
                                  << "Invalid value for parameter " << spname << ": " << value
                                  << " is not " << predicate::description << " " << bound);
            }
            return Status::OK();
        });
    }

    void appendConstraints(BSONObjBuilder* b) const {
        if (_bounds.empty()) {
            return;
        }
        BSONArrayBuilder arr(b->subarrayStart("bounds"));
        for (const auto& [kind, value] : _bounds) {
            BSONObjBuilder entry(arr.subobjStart());
            entry.append("kind", kind);
            entry.append("value", value);
        }
    }

private:
    struct Bound {
        std::string_view kind;
        element_type value;
    };

    SW _storage;

    std::vector<std::function<validator_t>> _validators;
    std::vector<Bound> _bounds;
    std::function<onUpdate_t> _onUpdate;
    std::once_flag _setDefaultOnce;
};

template <typename Storage>
using ClusterParameterWithStorage [[MONGO_MOD_PUBLIC]] =
    IDLServerParameterWithStorage<ServerParameterType::kClusterWide, Storage>;
}  // namespace mongo
