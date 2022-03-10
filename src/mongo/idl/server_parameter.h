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
#include "mongo/db/logical_time.h"

#define MONGO_SERVER_PARAMETER_REGISTER(name) \
    MONGO_INITIALIZER_GENERAL(                \
        name, ("BeginServerParameterRegistration"), ("EndServerParameterRegistration"))

namespace mongo {

/**
 * How and when a given Server Parameter may be set/modified.
 */
enum class ServerParameterType {
    /**
     * May not be set at any time.
     * Used as a means to read out current state, similar to ServerStatus.
     */
    kReadOnly,

    /**
     * Parameter can only be set via `{setParameter: 1, name: value}`
     */
    kRuntimeOnly,

    /**
     * Parameter can only be set via `--setParameter 'name=value'`,
     * and is only read at startup after command-line
     * parameters, and the config file are processed.
     */
    kStartupOnly,

    /**
     * Parameter can be set at both startup and runtime.
     * This is essentially a union of kRuntimeOnly and kStartupOnly.
     */
    kStartupAndRuntime,

    /**
     * Cluster-wide configuration setting.
     * These are by-definition runtime settable only, however unlike other modes (including
     * kRuntimeOnly), these are set via the {setClusterParameter:...} command and stored in a
     * separate map. ClusterWide settings are propagated to other nodes in the cluster.
     */
    kClusterWide,
};

class ServerParameterSet;
class OperationContext;

class ServerParameter {
public:
    using Map = std::map<std::string, ServerParameter*>;

    ServerParameter(StringData name, ServerParameterType spt);
    virtual ~ServerParameter() = default;

    std::string name() const {
        return _name;
    }

    /**
     * @return if you can set on command line or config file
     */
    bool allowedToChangeAtStartup() const {
        return (_type == ServerParameterType::kStartupOnly) ||
            (_type == ServerParameterType::kStartupAndRuntime);
    }

    /**
     * @param if you can use (get|set)Parameter
     */
    bool allowedToChangeAtRuntime() const {
        return (_type == ServerParameterType::kRuntimeOnly) ||
            (_type == ServerParameterType::kStartupAndRuntime) ||
            (_type == ServerParameterType::kClusterWide);
    }

    ServerParameterType getServerParameterType() const {
        return _type;
    }

    bool isClusterWide() const {
        return (_type == ServerParameterType::kClusterWide);
    }

    bool isNodeLocal() const {
        return (_type != ServerParameterType::kClusterWide);
    }

    LogicalTime getClusterParameterTime() const {
        return _clusterParameterTime;
    }

    void setClusterParameterTime(const LogicalTime& clusterParameterTime);

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) = 0;

    virtual void appendSupportingRoundtrip(OperationContext* opCtx,
                                           BSONObjBuilder& b,
                                           const std::string& name) {
        append(opCtx, b, name);
    }

    virtual Status validate(const BSONElement& newValueElement) const {
        return Status::OK();
    }

    // This base implementation calls `setFromString(coerceToString(newValueElement))`.
    // Derived classes may customize the behavior by specifying `override_set` in IDL.
    virtual Status set(const BSONElement& newValueElement);

    virtual Status setFromString(const std::string& str) = 0;

    bool isTestOnly() const {
        return _testOnly;
    }

    void setTestOnly() {
        _testOnly = true;
    }

    bool isRedact() const {
        return _redact;
    }

    void setRedact() {
        _redact = true;
    }

protected:
    // Helper for translating setParameter values from BSON to string.
    StatusWith<std::string> _coerceToString(const BSONElement&);

private:
    std::string _name;
    LogicalTime _clusterParameterTime;
    ServerParameterType _type;
    bool _testOnly = false;
    bool _redact = false;
};

class ServerParameterSet {
public:
    using Map = ServerParameter::Map;

    void add(ServerParameter* sp);
    void remove(const std::string& name);

    const Map& getMap() const {
        return _map;
    }

    void disableTestParameters();

    template <typename T = ServerParameter>
    T* getIfExists(StringData name) {
        const auto& it = _map.find(name.toString());
        if (it == _map.end()) {
            return nullptr;
        }
        return checked_cast<T*>(it->second);
    }

    template <typename T = ServerParameter>
    T* get(StringData name) {
        T* ret = getIfExists<T>(name);
        uassert(ErrorCodes::NoSuchKey, str::stream() << "Unknown server parameter: " << name, ret);
        return ret;
    }

    // A ServerParameterSet can be picky about which ServerParameters can be
    // added to it. `func` will be called whenever a `ServerParameter` is added
    // to this set. It will throw to reject that ServerParameter. This can be
    // because of ServerParameterType, or other criteria.
    void setValidate(std::function<void(ServerParameter*)> func) {
        _validate = std::move(func);
    }

    // Singleton instances of ServerParameterSet
    // used for retrieving the local or cluster-wide maps.
    static ServerParameterSet* getNodeParameterSet();
    static ServerParameterSet* getClusterParameterSet();
    static ServerParameterSet* getParameterSet(ServerParameterType spt) {
        if (spt == ServerParameterType::kClusterWide) {
            return getClusterParameterSet();
        } else {
            return getNodeParameterSet();
        }
    }

private:
    std::function<void(ServerParameter*)> _validate;
    Map _map;
};

void registerServerParameter(ServerParameter* p);

// Create an instance of Param, which must be derived from ServerParameter,
// and register it with a ServerParameterSet.
template <typename Param>
Param* makeServerParameter(StringData name, ServerParameterType spt) {
    auto p = std::make_unique<Param>(std::string{name}, spt);
    registerServerParameter(&*p);
    return p.release();
}

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
    std::once_flag _warnOnce;
    ServerParameter* _sp;
};

inline IDLServerParameterDeprecatedAlias* makeIDLServerParameterDeprecatedAlias(
    StringData name, ServerParameter* sp) {
    auto p = std::make_unique<IDLServerParameterDeprecatedAlias>(name, sp);
    registerServerParameter(p.get());
    return p.release();
}

}  // namespace mongo
