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

#include <string>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

#define MONGO_SERVER_PARAMETER_REGISTER(name) \
    MONGO_INITIALIZER_GENERAL(                \
        name, ("BeginServerParameterRegistration"), ("EndServerParameterRegistration"))

namespace mongo {

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

class ServerParameterSet;
class OperationContext;

class ServerParameter {
public:
    using Map = std::map<std::string, ServerParameter*>;

    ServerParameter(StringData name, ServerParameterType spt);
    ServerParameter(ServerParameterSet* sps,
                    StringData name,
                    bool allowedToChangeAtStartup,
                    bool allowedToChangeAtRuntime);
    ServerParameter(ServerParameterSet* sps, StringData name);
    virtual ~ServerParameter() = default;

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

    bool isTestOnly() const {
        return _testOnly;
    }

    void setTestOnly() {
        _testOnly = true;
    }

private:
    std::string _name;
    bool _allowedToChangeAtStartup;
    bool _allowedToChangeAtRuntime;
    bool _testOnly = false;
};

class ServerParameterSet {
public:
    using Map = ServerParameter::Map;

    void add(ServerParameter* sp);

    const Map& getMap() const {
        return _map;
    }

    static ServerParameterSet* getGlobal();

    void disableTestParameters();

    template <typename T = ServerParameter>
    T* get(StringData name) {
        const auto& it = _map.find(name.toString());
        uassert(ErrorCodes::NoSuchKey,
                str::stream() << "Unknown server parameter: " << name,
                it != _map.end());
        return checked_cast<T*>(it->second);
    }

private:
    Map _map;
};

/**
 * Proxy instance for deprecated aliases of set parameters.
 */
class IDLServerParameterDeprecatedAlias : public ServerParameter {
public:
    IDLServerParameterDeprecatedAlias(StringData name, ServerParameter* sp);

    void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) final;
    Status set(const BSONElement& newValueElement) final;
    Status setFromString(const std::string& str) final;

private:
    ServerParameter* _sp;
};

}  // namespace mongo
