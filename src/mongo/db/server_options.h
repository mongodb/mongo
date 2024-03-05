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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <ctime>
#include <string>
#include <variant>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/cluster_role.h"
#include "mongo/logv2/log_format.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/version/releases.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace mongo {

const int DEFAULT_UNIX_PERMS = 0700;
constexpr size_t DEFAULT_MAX_CONN = 1000000;

struct ServerGlobalParams {
    std::string binaryName;  // mongod or mongos
    std::string cwd;         // cwd of when process started

    int port = DefaultDBPort;  // --port
    enum {
        RouterPort = 27016,
        DefaultDBPort = 27017,
        ShardServerPort = 27018,
        ConfigServerPort = 27019,
        CryptDServerPort = 27020,
// TODO: SERVER-80343 Remove this ifdef once gRPC is compiled on all variants
#ifdef MONGO_CONFIG_GRPC
        DefaultGRPCServerPort = 27021,
#endif
        DefaultMagicRestorePort = 27022,
    };

    enum MaintenanceMode { None, ReplicaSetMode, StandaloneMode };

    static std::string getPortSettingHelpText();

    std::vector<std::string> bind_ips;  // --bind_ip
    bool enableIPv6 = false;
    bool rest = false;  // --rest

    int listenBacklog = SOMAXCONN;  // --listenBacklog

    AtomicWord<bool> quiet{false};  // --quiet

    ClusterRole clusterRole = ClusterRole::None;  // --configsvr/--shardsvr
    MaintenanceMode maintenanceMode;              // --maintenanceMode

    bool upgradeBackCompat{false};    // --upgradeBackCompat
    bool downgradeBackCompat{false};  // --downgradeBackCompat

    boost::optional<int> routerPort;      // --routerPort
    bool doAutoBootstrapSharding{false};  // This is derived from other settings during startup.

    bool objcheck = true;  // --objcheck

    // Shell parameter, used for testing only, to tell the shell to crash on InvalidBSON errors.
    // Can be paired with --objcheck so that extra BSON validation occurs.
    bool crashOnInvalidBSONError = false;  // --crashOnInvalidBSONError

    int defaultProfile = 0;  // --profile
    boost::optional<BSONObj> defaultProfileFilter;
    AtomicWord<int> slowMS{100};           // --time in ms that is "slow"
    AtomicWord<double> sampleRate{1.0};    // --samplerate rate at which to sample slow queries
    int defaultLocalThresholdMillis = 15;  // --localThreshold in ms to consider a node local

    bool noUnixSocket = false;  // --nounixsocket
    bool doFork = false;        // --fork
    bool isMongoBridge = false;

    std::string socket = "/tmp";  // UNIX domain socket directory

    size_t maxConns = DEFAULT_MAX_CONN;  // Maximum number of simultaneous open connections.
    std::vector<std::variant<CIDR, std::string>> maxConnsOverride;
    int reservedAdminThreads = 0;

    int unixSocketPermissions = DEFAULT_UNIX_PERMS;  // permissions for the UNIX domain socket

    std::string keyFile;           // Path to keyfile, or empty if none.
    std::string pidFile;           // Path to pid file, or empty if none.
    std::string timeZoneInfoPath;  // Path to time zone info directory, or empty if none.

    std::string logpath;  // Path to log file, if logging to a file; otherwise, empty.
    logv2::LogTimestampFormat logTimestampFormat = logv2::LogTimestampFormat::kISO8601Local;

    bool logAppend = false;         // True if logging to a file in append mode.
    bool logRenameOnRotate = true;  // True if logging should rename log files on rotate
    bool logWithSyslog = false;     // True if logging to syslog; must not be set if logpath is set.
    int syslogFacility;             // Facility used when appending messages to the syslog.

#ifndef _WIN32
    int forkReadyFd = -1;  // for `--fork`. Write to it and close it when daemon service is up.
#endif

    time_t started = ::time(nullptr);

    BSONArray argvArray;
    BSONObj parsedOpts;

    enum AuthState { kEnabled, kDisabled, kUndefined };

    AuthState authState = AuthState::kUndefined;

    bool transitionToAuth = false;  // --transitionToAuth, mixed mode for rolling auth upgrade

    ClusterAuthMode startupClusterAuthMode;

    // for the YAML config, sharding._overrideShardIdentity. Can only be used when in
    // queryableBackupMode.
    BSONObj overrideShardIdentity;

    // True if the current binary version is an LTS Version.
    static constexpr bool kIsLTSBinaryVersion = false;

// TODO: SERVER-80343 Remove this ifdef once gRPC is compiled on all variants
#ifdef MONGO_CONFIG_GRPC
    int grpcPort = DefaultGRPCServerPort;
    int grpcServerMaxThreads = 1000;
#endif

    /**
     * Represents a "snapshot" of the in-memory FCV at a particular point in time.
     * This is useful for callers who need to perform multiple FCV checks and expect the checks to
     * be performed on a consistent (but possibly stale) FCV value.
     *
     * For example: checking isVersionInitialized() && isLessThan() with the same FCVSnapshot value
     * would have the guarantee that if the FCV value is initialized during isVersionInitialized(),
     * it will still be during isLessThan().
     *
     * Note that this can get stale, so if you call acquireFCVSnapshot() once, and then update
     * serverGlobalParams.mutableFCV, and expect to use the new FCV value, you must acquire another
     * snapshot. In general, if you want to check multiple properties of the FCV at a specific point
     * in time, you should use one snapshot. For example, if you want to check both
     * that the FCV is initialized and if it's less than some version, and that featureFlagXX is
     * enabled on this FCV, this should all be using the same FCVSnapshot of the FCV value.
     *
     * But if you're doing multiple completely separate FCV checks at different points in time, such
     * as over multiple functions, or multiple distinct feature flag enablement checks (i.e.
     * featureFlagXX.isEnabled && featureFlagYY.isEnabled), you should get a new FCV snapshot for
     * each check since the old one may be stale.
     */
    struct FCVSnapshot {
        using FCV = multiversion::FeatureCompatibilityVersion;

        /**
         * Creates an immutable "snapshot" of the passed in FCV.
         */
        explicit FCVSnapshot(FCV version) : _version(version) {}

        /**
         * On startup, the featureCompatibilityVersion may not have been explicitly set yet. This
         * exposes the actual state of the featureCompatibilityVersion if it is uninitialized.
         */
        bool isVersionInitialized() const {
            return _version != FCV::kUnsetDefaultLastLTSBehavior;
        }

        /**
         * This safe getter for the featureCompatibilityVersion parameter ensures the parameter has
         * been initialized with a meaningful value.
         */
        FCV getVersion() const {
            invariant(isVersionInitialized());
            return _version;
        }

        bool isLessThanOrEqualTo(FCV version, FCV* versionReturn = nullptr) const {
            auto currentVersion = getVersion();
            if (versionReturn != nullptr) {
                *versionReturn = currentVersion;
            }
            return currentVersion <= version;
        }

        bool isGreaterThanOrEqualTo(FCV version, FCV* versionReturn = nullptr) const {
            auto currentVersion = getVersion();
            if (versionReturn != nullptr) {
                *versionReturn = currentVersion;
            }
            return currentVersion >= version;
        }

        bool isLessThan(FCV version, FCV* versionReturn = nullptr) const {
            auto currentVersion = getVersion();
            if (versionReturn != nullptr) {
                *versionReturn = currentVersion;
            }
            return currentVersion < version;
        }

        bool isGreaterThan(FCV version, FCV* versionReturn = nullptr) const {
            auto currentVersion = getVersion();
            if (versionReturn != nullptr) {
                *versionReturn = currentVersion;
            }
            return currentVersion > version;
        }

        // This function is to be used for generic FCV references only, and not for FCV-gating.
        bool isUpgradingOrDowngrading() const {
            return isUpgradingOrDowngrading(getVersion());
        }

        static bool isUpgradingOrDowngrading(FCV version) {
            // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
            return version != multiversion::GenericFCV::kLatest &&
                version != multiversion::GenericFCV::kLastContinuous &&
                version != multiversion::GenericFCV::kLastLTS;
        }

        /**
         * Logs the current FCV global state.
         * context: the context in which this function was called, to differentiate logs (e.g.
         * startup, log rotation).
         */
        void logFCVWithContext(StringData context) const;

    private:
        const FCV _version;
    };

    /**
     * Represents the in-memory FCV that can be changed.
     * This should only be used to set/reset the in-memory FCV, or get a snapshot of the current
     * in-memory FCV. It *cannot* be used to check the actual value of the in-memory FCV, and in
     * particular, check the value over multiple function calls. This is because the in-memory FCV
     * might change in between those calls (such as during initial sync). Instead, use
     * acquireFCVSnapshot() to get a snapshot of the FCV, and use the functions available on
     * FCVSnapshot.
     */
    struct MutableFCV {
        using FCV = multiversion::FeatureCompatibilityVersion;

        /**
         * Gets an immutable copy/snapshot of the current FCV so that callers can check
         * the FCV value at a particular point in time over multiple function calls
         * without the snapshot value changing.
         * Note that this snapshot might be when the FCV is uninitialized, which could happen
         * during initial sync. The caller of this function and the subsequent functions
         * within FCVSnapshot must handle that case.
         */
        FCVSnapshot acquireFCVSnapshot() const {
            return FCVSnapshot(_version.load());
        }

        void reset() {
            _version.store(FCV::kUnsetDefaultLastLTSBehavior);
        }

        void setVersion(FCV version) {
            return _version.store(version);
        }

    private:
        AtomicWord<FCV> _version{FCV::kUnsetDefaultLastLTSBehavior};

    } mutableFCV;

    // Const reference for featureCompatibilityVersion checks.
    const MutableFCV& featureCompatibility = mutableFCV;

    // Feature validation differs depending on the role of a mongod in a replica set. Replica set
    // primaries can accept user-initiated writes and validate based on the feature compatibility
    // version. A secondary always validates in the upgraded mode so that it can sync new features,
    // even when in the downgraded feature compatibility mode.
    AtomicWord<bool> validateFeaturesAsPrimary{true};

    std::vector<std::string> disabledSecureAllocatorDomains;

    bool enableMajorityReadConcern = true;
};

extern ServerGlobalParams serverGlobalParams;

template <typename NameTrait>
struct TraitNamedDomain {
    static bool peg() {
        const auto& dsmd = serverGlobalParams.disabledSecureAllocatorDomains;
        const auto contains = [&](StringData dt) {
            return std::find(dsmd.begin(), dsmd.end(), dt) != dsmd.end();
        };
        static const bool ret = !(contains("*"_sd) || contains(NameTrait::DomainType));
        return ret;
    }
};

}  // namespace mongo
