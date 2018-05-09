// server_parameters.h

/**
*    Copyright (C) 2012 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <map>
#include <string>
#include <vector>

#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/stringutils.h"

namespace mongo {

class ServerParameterSet;
class OperationContext;

/**
 * Lets you make server level settings easily configurable.
 * Hooks into (set|get)Parameter, as well as command line processing
 *
 * NOTE: ServerParameters set at runtime can be read or written to at anytime, and are not
 * thread-safe without atomic types or other concurrency techniques.
 */
class ServerParameter {
public:
    typedef std::map<std::string, ServerParameter*> Map;

    ServerParameter(ServerParameterSet* sps,
                    const std::string& name,
                    bool allowedToChangeAtStartup,
                    bool allowedToChangeAtRuntime);
    ServerParameter(ServerParameterSet* sps, const std::string& name);
    virtual ~ServerParameter();

    std::string name() const {
        return _name;
    }

    /**
     * @return if you can set on command line or config file
     */
    bool allowedToChangeAtStartup() const {
        return _allowedToChangeAtStartup;
    }

    /**
     * @param if you can use (get|set)Parameter
     */
    bool allowedToChangeAtRuntime() const {
        return _allowedToChangeAtRuntime;
    }


    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) = 0;

    virtual Status set(const BSONElement& newValueElement) = 0;

    virtual Status setFromString(const std::string& str) = 0;

private:
    std::string _name;
    bool _allowedToChangeAtStartup;
    bool _allowedToChangeAtRuntime;
};

class ServerParameterSet {
public:
    typedef std::map<std::string, ServerParameter*> Map;

    void add(ServerParameter* sp);

    const Map& getMap() const {
        return _map;
    }

    static ServerParameterSet* getGlobal();

private:
    Map _map;
};

/**
 * Server Parameters can be set startup up and/or runtime.
 *
 * At startup, --setParameter ... or config file is used.
 * At runtime, { setParameter : 1, ...} is used.
 */
enum class ServerParameterType {

    /**
     * Parameter can only be set via runCommand.
     */
    kRuntimeOnly,

    /**
     * Parameter can only be set via --setParameter, and is only read at startup after command-line
     * parameters, and the config file are processed.
     */
    kStartupOnly,

    /**
     * Parameter can be set at both startup and runtime.
     */
    kStartupAndRuntime,
};

/**
 * Lets you make server level settings easily configurable.
 * Hooks into (set|get)Parameter, as well as command line processing
 */
template <typename T>
class BoundServerParameter : public ServerParameter {
private:
    using setter = stdx::function<Status(const T&)>;
    using getter = stdx::function<T()>;
    using SPT = ServerParameterType;

public:
    BoundServerParameter(const std::string& name,
                         const setter set,
                         const getter get,
                         SPT paramType = SPT::kStartupOnly)
        : BoundServerParameter(ServerParameterSet::getGlobal(), name, set, get, paramType) {}

    BoundServerParameter(ServerParameterSet* sps,
                         const std::string& name,
                         const setter set,
                         const getter get,
                         SPT paramType = SPT::kStartupOnly)
        : ServerParameter(sps,
                          name,
                          paramType == SPT::kStartupOnly || paramType == SPT::kStartupAndRuntime,
                          paramType == SPT::kRuntimeOnly || paramType == SPT::kStartupAndRuntime),
          _setter(set),
          _getter(get) {}
    ~BoundServerParameter() override = default;

    void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) override {
        b.append(name, _getter());
    }

    Status set(const BSONElement& newValueElement) override {
        T newValue;

        if (!newValueElement.coerce(&newValue)) {
            return Status(ErrorCodes::BadValue, "Can't coerce value");
        }

        return _setter(newValue);
    }

    Status setFromString(const std::string& str) override;

private:
    const setter _setter;
    const getter _getter;
};

template <>
inline Status BoundServerParameter<bool>::setFromString(const std::string& str) {
    if ((str == "1") || (str == "true")) {
        return _setter(true);
    }
    if ((str == "0") || (str == "false")) {
        return _setter(false);
    }
    return Status(ErrorCodes::BadValue, "Value is not a valid boolean");
}

template <>
inline Status BoundServerParameter<std::string>::setFromString(const std::string& str) {
    return _setter(str);
}

template <>
inline Status BoundServerParameter<std::vector<std::string>>::setFromString(
    const std::string& str) {
    std::vector<std::string> v;
    splitStringDelim(str, &v, ',');
    return _setter(v);
}

template <typename T>
inline Status BoundServerParameter<T>::setFromString(const std::string& str) {
    T value;
    Status status = parseNumberFromString(str, &value);
    if (!status.isOK()) {
        return status;
    }
    return _setter(value);
}

template <typename T>
class LockedServerParameter : public BoundServerParameter<T> {
private:
    using SPT = ServerParameterType;

public:
    LockedServerParameter(const std::string& name,
                          const T& initval,
                          SPT paramType = SPT::kStartupAndRuntime)
        : LockedServerParameter(ServerParameterSet::getGlobal(), name, initval, paramType) {}

    LockedServerParameter(ServerParameterSet* sps,
                          const std::string& name,
                          const T& initval,
                          SPT paramType = SPT::kStartupAndRuntime)
        : BoundServerParameter<T>(sps,
                                  name,
                                  [this](const T& v) { return setLocked(v); },
                                  [this]() { return getLocked(); },
                                  paramType),
          _value(initval) {}
    ~LockedServerParameter() override = default;

    Status setLocked(const T& value) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _value = value;
        return Status::OK();
    }

    T getLocked() const {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        return _value;
    }

private:
    mutable stdx::mutex _mutex;
    T _value;
};

/**
 * Type trait for ServerParameterType to identify which types are safe to use at runtime because
 * they have std::atomic or equivalent types.
 */
template <typename T>
class is_safe_runtime_parameter_type : public std::false_type {};

template <>
class is_safe_runtime_parameter_type<bool> : public std::true_type {};

template <>
class is_safe_runtime_parameter_type<int> : public std::true_type {};

template <>
class is_safe_runtime_parameter_type<long long> : public std::true_type {};

template <>
class is_safe_runtime_parameter_type<double> : public std::true_type {};

/**
 * Get the type of storage to use for a given tuple of <type, ServerParameterType>.
 *
 * By default, we want std::atomic or equivalent types because they are thread-safe.
 * If the parameter is a startup only type, then there are no concurrency concerns since
 * server parameters are processed on the main thread while it is single-threaded during startup.
 */
template <typename T, ServerParameterType paramType>
class server_parameter_storage_type {
public:
    using value_type = AtomicWord<T>;
    static T get(value_type* v) {
        return v->load();
    }
    static void set(value_type* v, const T& newValue) {
        v->store(newValue);
    }
};

template <typename T>
class server_parameter_storage_type<T, ServerParameterType::kStartupOnly> {
public:
    using value_type = T;
    static T get(value_type* v) {
        return *v;
    }
    static void set(value_type* v, const T& newValue) {
        *v = newValue;
    }
};

template <>
class server_parameter_storage_type<double, ServerParameterType::kRuntimeOnly> {
public:
    using value_type = AtomicDouble;
    static double get(value_type* v) {
        return v->load();
    }
    static void set(value_type* v, const double& newValue) {
        v->store(newValue);
    }
};

template <>
class server_parameter_storage_type<double, ServerParameterType::kStartupAndRuntime> {
public:
    using value_type = AtomicDouble;
    static double get(value_type* v) {
        return v->load();
    }
    static void set(value_type* v, const double& newValue) {
        v->store(newValue);
    }
};

/**
 * Implementation of BoundServerParameter for reading and writing a server parameter with a given
 * name and type into a specific C++ variable.
 *
 * NOTE: ServerParameters set at runtime can be read or written to at anytime, and are not
 * thread-safe without atomic types or other concurrency techniques.
 */
template <typename T, ServerParameterType paramType>
class ExportedServerParameter : public BoundServerParameter<T> {
public:
    MONGO_STATIC_ASSERT_MSG(paramType == ServerParameterType::kStartupOnly ||
                                is_safe_runtime_parameter_type<T>::value,
                            "This type is not supported as a runtime server parameter.");

    using storage_type = typename server_parameter_storage_type<T, paramType>::value_type;

    /**
     * Construct an ExportedServerParameter in parameter set "sps", named "name", whose storage
     * is at "value".
     *
     * If allowedToChangeAtStartup is true, the parameter may be set at the command line,
     * e.g. via the --setParameter switch.  If allowedToChangeAtRuntime is true, the parameter
     * may be set at runtime, e.g.  via the setParameter command.
     */
    ExportedServerParameter(ServerParameterSet* sps, const std::string& name, storage_type* value)
        : BoundServerParameter<T>(
              sps,
              name,
              [this](const T& v) { return set(v); },
              [this] { return server_parameter_storage_type<T, paramType>::get(_value); },
              paramType),
          _value(value) {}
    ~ExportedServerParameter() override {}

    // Don't let the template method hide our inherited method
    Status set(const BSONElement& newValueElement) override {
        return BoundServerParameter<T>::set(newValueElement);
    }

    virtual Status set(const T& newValue) {
        auto const status = validate(newValue);
        if (!status.isOK()) {
            return status;
        }
        server_parameter_storage_type<T, paramType>::set(_value, newValue);
        return Status::OK();
    }

protected:
    virtual Status validate(const T& potentialNewValue) {
        return Status::OK();
    }

    storage_type* const _value;  // owned elsewhere
};

/**
 * An exported server parameter with a validation function.
 */
template <typename T, ServerParameterType paramType>
class ExportedServerParameterWithValidator : public ExportedServerParameter<T, paramType> {
public:
    using storage_type = typename server_parameter_storage_type<T, paramType>::value_type;
    using validator_function = stdx::function<Status(const T&)>;

    ExportedServerParameterWithValidator(ServerParameterSet* sps,
                                         const std::string& name,
                                         storage_type* value,
                                         validator_function validator)
        : ExportedServerParameter<T, paramType>(sps, name, value),
          _validator(std::move(validator)) {}

protected:
    Status validate(const T& potentialNewValue) final {
        return _validator(potentialNewValue);
    }

private:
    validator_function _validator;
};
}  // namespace mongo


#define MONGO_EXPORT_SERVER_PARAMETER_IMPL(NAME, TYPE, INITIAL_VALUE, PARAM_TYPE)    \
    server_parameter_storage_type<TYPE, PARAM_TYPE>::value_type NAME(INITIAL_VALUE); \
    ExportedServerParameter<TYPE, PARAM_TYPE> _##NAME(ServerParameterSet::getGlobal(), #NAME, &NAME)

/**
 * Create a global variable of type "TYPE" named "NAME" with the given INITIAL_VALUE.  The
 * value may be set at startup or at runtime.
 */
#define MONGO_EXPORT_SERVER_PARAMETER(NAME, TYPE, INITIAL_VALUE) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL(                          \
        NAME, TYPE, INITIAL_VALUE, ServerParameterType::kStartupAndRuntime)

/**
 * Like MONGO_EXPORT_SERVER_PARAMETER, but the value may only be set at startup.
 */
#define MONGO_EXPORT_STARTUP_SERVER_PARAMETER(NAME, TYPE, INITIAL_VALUE) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL(NAME, TYPE, INITIAL_VALUE, ServerParameterType::kStartupOnly)

/**
 * Like MONGO_EXPORT_SERVER_PARAMETER, but the value may only be set at runtime.
 */
#define MONGO_EXPORT_RUNTIME_SERVER_PARAMETER(NAME, TYPE, INITIAL_VALUE) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL(NAME, TYPE, INITIAL_VALUE, ServerParameterType::kRuntimeOnly)


/**
 * Copies of above macros, but with the ability to pass in a callable to validate the input
 */
#define MONGO_EXPORT_SERVER_PARAMETER_IMPL_WITH_VALIDATOR(                           \
    NAME, TYPE, INITIAL_VALUE, VALIDATOR, PARAM_TYPE)                                \
    server_parameter_storage_type<TYPE, PARAM_TYPE>::value_type NAME(INITIAL_VALUE); \
    ExportedServerParameterWithValidator<TYPE, PARAM_TYPE> _##NAME(                  \
        ServerParameterSet::getGlobal(), #NAME, &NAME, VALIDATOR)


#define MONGO_EXPORT_SERVER_PARAMETER_WITH_VALIDATOR(NAME, TYPE, INITIAL_VALUE, VALIDATOR) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL_WITH_VALIDATOR(                                     \
        NAME, TYPE, INITIAL_VALUE, VALIDATOR, ServerParameterType::kStartupAndRuntime)

#define MONGO_EXPORT_STARTUP_SERVER_PARAMETER_WITH_VALIDATOR(NAME, TYPE, INITIAL_VALUE, VALIDATOR) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL_WITH_VALIDATOR(                                             \
        NAME, TYPE, INITIAL_VALUE, VALIDATOR, ServerParameterType::kStartupOnly)

#define MONGO_EXPORT_RUNTIME_SERVER_PARAMETER_WITH_VALIDATOR(NAME, TYPE, INITIAL_VALUE, VALIDATOR) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL_WITH_VALIDATOR(                                             \
        NAME, TYPE, INITIAL_VALUE, VALIDATOR, ServerParameterType::kRuntimeOnly)
