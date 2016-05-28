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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_proxy.h"

namespace mongo {

class ServerParameterSet;
class OperationContext;

/**
 * Lets you make server level settings easily configurable.
 * Hooks into (set|get)Paramter, as well as command line processing
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


    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) = 0;

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
    using value_type = std::atomic<T>;  // NOLINT
};

template <typename T>
class server_parameter_storage_type<T, ServerParameterType::kStartupOnly> {
public:
    using value_type = T;
};

template <>
class server_parameter_storage_type<double, ServerParameterType::kRuntimeOnly> {
public:
    using value_type = AtomicDouble;
};

template <>
class server_parameter_storage_type<double, ServerParameterType::kStartupAndRuntime> {
public:
    using value_type = AtomicDouble;
};

/**
 * Implementation of ServerParameter for reading and writing a server parameter with a given
 * name and type into a specific C++ variable.
 *
 * NOTE: ServerParameters set at runtime can be read or written to at anytime, and are not
 * thread-safe without atomic types or other concurrency techniques.
 */
template <typename T, ServerParameterType paramType>
class ExportedServerParameter : public ServerParameter {
public:
    static_assert(paramType == ServerParameterType::kStartupOnly ||
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
        : ServerParameter(sps,
                          name,
                          paramType == ServerParameterType::kStartupOnly ||
                              paramType == ServerParameterType::kStartupAndRuntime,
                          paramType == ServerParameterType::kRuntimeOnly ||
                              paramType == ServerParameterType::kStartupAndRuntime),
          _value(value) {}
    virtual ~ExportedServerParameter() {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b.append(name, *_value);
    }

    virtual Status set(const BSONElement& newValueElement);
    virtual Status set(const T& newValue);

    virtual Status setFromString(const std::string& str);

protected:
    virtual Status validate(const T& potentialNewValue) {
        return Status::OK();
    }

    storage_type* const _value;  // owned elsewhere
};
}

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

#include "server_parameters_inline.h"
