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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/cluster_role.h"
#include "mongo/logv2/log_format.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/version/releases.h"

namespace mongo {

const int DEFAULT_UNIX_PERMS = 0700;
constexpr size_t DEFAULT_MAX_CONN = 1000000;

struct ServerGlobalParams {
    std::string binaryName;  // mongod or mongos
    std::string cwd;         // cwd of when process started

    int port = DefaultDBPort;  // --port
    enum {
        ConfigServerPort = 27019,
        CryptDServerPort = 27020,
        DefaultDBPort = 27017,
        ShardServerPort = 27018,
    };

    static std::string getPortSettingHelpText();

    std::vector<std::string> bind_ips;  // --bind_ip
    bool enableIPv6 = false;
    bool rest = false;  // --rest

    int listenBacklog = 0;  // --listenBacklog, real default is SOMAXCONN

    AtomicWord<bool> quiet{false};  // --quiet

    ClusterRole clusterRole = ClusterRole::None;  // --configsvr/--shardsvr

    bool objcheck = true;  // --objcheck

    // Shell parameter, used for testing only, to tell the shell to crash on InvalidBSON errors.
    // Can be paired with --objcheck so that extra BSON validation occurs.
    bool crashOnInvalidBSONError = false;  // --crashOnInvalidBSONError

    int defaultProfile = 0;  // --profile
    boost::optional<BSONObj> defaultProfileFilter;
    AtomicWord<int> slowMS{100};           // --time in ms that is "slow"
    AtomicWord<double> sampleRate{1.0};    // --samplerate rate at which to sample slow queries
    int defaultLocalThresholdMillis = 15;  // --localThreshold in ms to consider a node local
    bool moveParanoia = false;             // for move chunk paranoia

    bool noUnixSocket = false;    // --nounixsocket
    bool doFork = false;          // --fork
    std::string socket = "/tmp";  // UNIX domain socket directory
    std::string transportLayer;   // --transportLayer (must be either "asio" or "legacy")

    size_t maxConns = DEFAULT_MAX_CONN;  // Maximum number of simultaneous open connections.
    std::vector<stdx::variant<CIDR, std::string>> maxConnsOverride;
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

    struct FeatureCompatibility {
        using FCV = multiversion::FeatureCompatibilityVersion;

        /**
         * On startup, the featureCompatibilityVersion may not have been explicitly set yet. This
         * exposes the actual state of the featureCompatibilityVersion if it is uninitialized.
         */
        bool isVersionInitialized() const {
            return _version.load() != FCV::kUnsetDefaultLastLTSBehavior;
        }

        /**
         * This safe getter for the featureCompatibilityVersion parameter ensures the parameter has
         * been initialized with a meaningful value.
         */
        FCV getVersion() const {
            invariant(isVersionInitialized());
            return _version.load();
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
        bool isUpgradingOrDowngrading(boost::optional<FCV> version = boost::none) const {
            if (version == boost::none) {
                version = getVersion();
            }

            // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
            return version != multiversion::GenericFCV::kLatest &&
                version != multiversion::GenericFCV::kLastContinuous &&
                version != multiversion::GenericFCV::kLastLTS;
        }

        void reset() {
            _version.store(FCV::kUnsetDefaultLastLTSBehavior);
        }

        void setVersion(FCV version) {
            return _version.store(version);
        }

        /**
         * Logs the current FCV global state.
         * context: the context in which this function was called, to differentiate logs (e.g.
         * startup, log rotation).
         */
        void logFCVWithContext(StringData context) const;

    private:
        AtomicWord<FCV> _version{FCV::kUnsetDefaultLastLTSBehavior};

    } mutableFeatureCompatibility;

    // Const reference for featureCompatibilityVersion checks.
    const FeatureCompatibility& featureCompatibility = mutableFeatureCompatibility;

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
