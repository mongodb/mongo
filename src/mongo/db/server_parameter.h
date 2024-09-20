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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/version/releases.h"

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

class FeatureFlag;
class ServerParameterSet;

class OperationContext;

template <typename U>
using TenantIdMap = std::map<boost::optional<TenantId>, U>;

class ServerParameter {
public:
    using Map = std::map<std::string, std::unique_ptr<ServerParameter>, std::less<>>;

    ServerParameter(StringData name, ServerParameterType spt);
    ServerParameter(const ServerParameter& rhs);
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

    virtual void append(OperationContext* opCtx,
                        BSONObjBuilder* b,
                        StringData name,
                        const boost::optional<TenantId>& tenantId) = 0;

    virtual void appendSupportingRoundtrip(OperationContext* opCtx,
                                           BSONObjBuilder* b,
                                           StringData name,
                                           const boost::optional<TenantId>& tenantId) {
        append(opCtx, b, name, tenantId);
    }

    virtual Status validate(const BSONElement& newValueElement,
                            const boost::optional<TenantId>& tenantId) const {
        return Status::OK();
    }

    Status validate(const BSONObj& newValueObj, const boost::optional<TenantId>& tenantId) const {
        return validate(BSON("" << newValueObj).firstElement(), tenantId);
    }

    // This base implementation calls `setFromString(coerceToString(newValueElement))`.
    // Derived classes may customize the behavior by specifying `override_set` in IDL.
    virtual Status set(const BSONElement& newValueElement,
                       const boost::optional<TenantId>& tenantId);

    /**
     * This method will reset the server parameter's value back to its default. This is currently
     * only used by cluster server parameters, but can work with node-only
     * IDLServerParameterWithStorage.
     * - IDLServerParameterWithStorage automatically initializes a copy of the storage variable's
     * initial value when it is constructed, which is treated as the default value. When the storage
     * variable is not declared by the IDL generator, it will use the setDefault() method to
     * adjust both the current value and the default value.
     * - Specialized server parameters can opt into providing resettability by implementing this
     * method. If it is called without being implemented, it will return an error via the inherited
     * method below.
     */
    virtual Status reset(const boost::optional<TenantId>& tenantId) {
        return Status{ErrorCodes::OperationFailed,
                      str::stream()
                          << "Parameter reset not implemented for server parameter: " << name()};
    }

    /**
     * Overload of set() that accepts BSONObjs instead of BSONElements. This is currently only used
     * for cluster server parameters but can be used for node-only server parameters.
     */
    Status set(const BSONObj& newValueObj, const boost::optional<TenantId>& tenantId) {
        return set(BSON("" << newValueObj).firstElement(), tenantId);
    }

    virtual Status setFromString(StringData str, const boost::optional<TenantId>& tenantId) = 0;

    /**
     * Simply returns the uninitialized/default-constructed LogicalTime by default.
     * IDLServerParameterWithStorage overrides this to atomically return the clusterParameterTime
     * stored in the base ClusterServerParameter class that all non-specialized cluster server
     * parameter storage types must be chained from. Specialized server parameters are expected to
     * implement a mechanism for atomically setting the clusterParameterTime in the set() method and
     * retrieving it via this method.
     */
    virtual LogicalTime getClusterParameterTime(const boost::optional<TenantId>& tenantId) const {
        return LogicalTime::kUninitialized;
    }

    bool isTestOnly() const {
        stdx::lock_guard lk(_mutex);
        return _testOnly;
    }

    void setTestOnly() {
        stdx::lock_guard lk(_mutex);
        _testOnly = true;
    }

    bool isRedact() const {
        stdx::lock_guard lk(_mutex);
        return _redact;
    }

    void setRedact() {
        stdx::lock_guard lk(_mutex);
        _redact = true;
    }

    bool isOmittedInFTDC() {
        stdx::lock_guard lk(_mutex);
        return _isOmittedInFTDC;
    }

    void setOmitInFTDC() {
        stdx::lock_guard lk(_mutex);
        _isOmittedInFTDC = true;
    }

private:
    enum DisableState { Enabled = 0, TemporarilyDisabled = 1, PermanentlyDisabled = 2 };

public:
    void disable(bool permanent) {
        stdx::lock_guard lk(_mutex);
        if (_disableState != DisableState::PermanentlyDisabled) {
            _disableState =
                permanent ? DisableState::PermanentlyDisabled : DisableState::TemporarilyDisabled;
        }
    }

    void enable() {
        stdx::lock_guard lk(_mutex);
        if (_disableState == DisableState::TemporarilyDisabled) {
            _disableState = DisableState::Enabled;
        }
    }

    bool isEnabled() const;

    // Return whether this server parameter would be enabled with the given FCV
    bool isEnabledOnVersion(const multiversion::FeatureCompatibilityVersion& targetFCV) const;

    // Return whether this server parameter is compatible with the given FCV, regardless of if it is
    // temporarily disabled
    bool canBeEnabledOnVersion(const multiversion::FeatureCompatibilityVersion& targetFCV) const;

    void setFeatureFlag(FeatureFlag* featureFlag) {
        stdx::lock_guard lk(_mutex);
        _featureFlag = featureFlag;
    }

    void setMinFCV(const multiversion::FeatureCompatibilityVersion& minFCV) {
        stdx::lock_guard lk(_mutex);
        _minFCV = minFCV;
    }

protected:
    virtual bool _isEnabledOnVersion(
        const multiversion::FeatureCompatibilityVersion& targetFCV) const;

    bool featureFlagIsDisabledOnVersion(
        const multiversion::FeatureCompatibilityVersion& targetFCV) const;

    bool minFCVIsLessThanOrEqualToVersion(
        const multiversion::FeatureCompatibilityVersion& fcv) const {
        stdx::lock_guard lk(_mutex);
        return !_minFCV || fcv >= *_minFCV;
    }

    // Helper for translating setParameter values from BSON to string.
    StatusWith<std::string> _coerceToString(const BSONElement&);

private:
    std::string _name;
    ServerParameterType _type;

    mutable stdx::mutex _mutex;

    bool _testOnly = false;
    bool _redact = false;
    bool _isOmittedInFTDC = false;
    FeatureFlag* _featureFlag = nullptr;
    boost::optional<multiversion::FeatureCompatibilityVersion> _minFCV = boost::none;

    // Tracks whether a parameter is enabled, temporarily disabled, or permanently disabled. This is
    // used when disabling (permanently) test-only parameters, and when enabling/disabling
    // (temporarily) cluster parameters on the mongos based on the cluster's FCV.
    DisableState _disableState = DisableState::Enabled;
};

class ServerParameterSet {
public:
    using Map = ServerParameter::Map;

    void add(std::unique_ptr<ServerParameter> sp);
    void remove(const std::string& name);

    const Map& getMap() const {
        return _map;
    }

    void disableTestParameters();

    template <typename T = ServerParameter>
    T* getIfExists(StringData name) const {
        const auto& it = _map.find(name);
        if (it == _map.end()) {
            return nullptr;
        }
        return checked_cast<T*>(it->second.get());
    }

    template <typename T = ServerParameter>
    T* get(StringData name) const {
        T* ret = getIfExists<T>(name);
        uassert(ErrorCodes::NoSuchKey, str::stream() << "Unknown server parameter: " << name, ret);
        return ret;
    }

    // A ServerParameterSet can be picky about which ServerParameters can be
    // added to it. `func` will be called whenever a `ServerParameter` is added
    // to this set. It will throw to reject that ServerParameter. This can be
    // because of ServerParameterType, or other criteria.
    void setValidate(std::function<void(const ServerParameter&)> func) {
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
    std::function<void(const ServerParameter&)> _validate;
    Map _map;
};

void registerServerParameter(std::unique_ptr<ServerParameter>);

/**
 * Proxy instance for deprecated aliases of set parameters.
 */
class IDLServerParameterDeprecatedAlias : public ServerParameter {
public:
    IDLServerParameterDeprecatedAlias(StringData name, ServerParameter* sp);

    void append(OperationContext* opCtx,
                BSONObjBuilder* b,
                StringData name,
                const boost::optional<TenantId>& tenantId) final;
    Status reset(const boost::optional<TenantId>& tenantId) final;
    Status set(const BSONElement& newValueElement, const boost::optional<TenantId>& tenantId) final;
    Status setFromString(StringData str, const boost::optional<TenantId>& tenantId) final;

private:
    std::once_flag _warnOnce;
    ServerParameter* _sp;
};

namespace idl_server_parameter_detail {

template <typename T>
inline StatusWith<T> coerceFromString(StringData str) {
    T value;
    Status status = NumberParser{}(str, &value);
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
    str::splitStringDelim(str.toString(), &v, ',');
    return v;
}

}  // namespace idl_server_parameter_detail
}  // namespace mongo
