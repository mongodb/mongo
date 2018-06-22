/*    Copyright 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/net/cidr.h"

namespace mongo {

const int DEFAULT_UNIX_PERMS = 0700;
constexpr size_t DEFAULT_MAX_CONN = 1000000;

enum class ClusterRole { None, ShardServer, ConfigServer };

struct ServerGlobalParams {
    std::string binaryName;  // mongod or mongos
    std::string cwd;         // cwd of when process started

    int port = DefaultDBPort;  // --port
    enum { DefaultDBPort = 27017, ConfigServerPort = 27019, ShardServerPort = 27018 };
    bool isDefaultPort() const {
        return port == DefaultDBPort;
    }

    std::vector<std::string> bind_ips;  // --bind_ip
    bool enableIPv6 = false;
    bool rest = false;  // --rest

    int listenBacklog = 0;  // --listenBacklog, real default is SOMAXCONN

    bool indexBuildRetry = true;  // --noIndexBuildRetry

    AtomicBool quiet{false};  // --quiet

    ClusterRole clusterRole = ClusterRole::None;  // --configsvr/--shardsvr

    bool cpu = false;  // --cpu show cpu time periodically

    bool objcheck = true;  // --objcheck

    int defaultProfile = 0;                // --profile
    int slowMS = 100;                      // --time in ms that is "slow"
    double sampleRate = 1.0;               // --samplerate rate at which to sample slow queries
    int defaultLocalThresholdMillis = 15;  // --localThreshold in ms to consider a node local
    bool moveParanoia = false;             // for move chunk paranoia

    bool noUnixSocket = false;    // --nounixsocket
    bool doFork = false;          // --fork
    std::string socket = "/tmp";  // UNIX domain socket directory
    std::string transportLayer;   // --transportLayer (must be either "asio" or "legacy")

    // --serviceExecutor ("adaptive", "synchronous")
    std::string serviceExecutor;

    size_t maxConns = DEFAULT_MAX_CONN;  // Maximum number of simultaneous open connections.
    std::vector<stdx::variant<CIDR, std::string>> maxConnsOverride;
    int reservedAdminThreads = 0;

    int unixSocketPermissions = DEFAULT_UNIX_PERMS;  // permissions for the UNIX domain socket

    std::string keyFile;           // Path to keyfile, or empty if none.
    std::string pidFile;           // Path to pid file, or empty if none.
    std::string timeZoneInfoPath;  // Path to time zone info directory, or empty if none.

    std::string logpath;            // Path to log file, if logging to a file; otherwise, empty.
    bool logAppend = false;         // True if logging to a file in append mode.
    bool logRenameOnRotate = true;  // True if logging should rename log files on rotate
    bool logWithSyslog = false;     // True if logging to syslog; must not be set if logpath is set.
    int syslogFacility;             // Facility used when appending messages to the syslog.

#ifndef _WIN32
    ProcessId parentProc;  // --fork pid of initial process
    ProcessId leaderProc;  // --fork pid of leader process
#endif

    /**
     * Switches to enable experimental (unsupported) features.
     */
    struct ExperimentalFeatures {
        ExperimentalFeatures() : storageDetailsCmdEnabled(false) {}
        bool storageDetailsCmdEnabled;  // -- enableExperimentalStorageDetailsCmd
    } experimental;

    time_t started = ::time(0);

    BSONArray argvArray;
    BSONObj parsedOpts;

    enum AuthState { kEnabled, kDisabled, kUndefined };

    AuthState authState = AuthState::kUndefined;

    bool transitionToAuth = false;  // --transitionToAuth, mixed mode for rolling auth upgrade
    AtomicInt32 clusterAuthMode;    // --clusterAuthMode, the internal cluster auth mode

    enum ClusterAuthModes {
        ClusterAuthMode_undefined,
        /**
        * Authenticate using keyfile, accept only keyfiles
        */
        ClusterAuthMode_keyFile,

        /**
        * Authenticate using keyfile, accept both keyfiles and X.509
        */
        ClusterAuthMode_sendKeyFile,

        /**
        * Authenticate using X.509, accept both keyfiles and X.509
        */
        ClusterAuthMode_sendX509,

        /**
        * Authenticate using X.509, accept only X.509
        */
        ClusterAuthMode_x509
    };

    // for the YAML config, sharding._overrideShardIdentity. Can only be used when in
    // queryableBackupMode.
    BSONObj overrideShardIdentity;

    struct FeatureCompatibility {
        /**
         * The combination of the fields in the admin.system.version document in the format
         * (version, targetVersion) are represented by this enum and determine this node's behavior.
         *
         * The legal enum (and featureCompatiblityVersion document) states are:
         *
         * kFullyDowngradedTo34
         * (3.4, Unset): Only 3.4 features are available, and new and existing storage
         *               engine entries use the 3.4 format
         *
         * kUpgradingTo36
         * (3.4, 3.6): Only 3.4 features are available, but new storage engine entries
         *             use the 3.6 format, and existing entries may have either the
         *             3.4 or 3.6 format
         *
         * kFullyUpgradedTo36
         * (3.6, Unset): 3.6 features are available, and new and existing storage
         *               engine entries use the 3.6 format
         *
         * kDowngradingTo34
         * (3.4, 3.4): Only 3.4 features are available and new storage engine
         *             entries use the 3.4 format, but existing entries may have
         *             either the 3.4 or 3.6 format
         *
         * kUnsetDefault34Behavior
         * (Unset, Unset): This is the case on startup before the fCV document is
         *                 loaded into memory. isVersionInitialized() will return
         *                 false, and getVersion() will return the default
         *                 (kFullyDowngradedTo34).
         *
         */
        enum class Version {
            kFullyDowngradedTo34,
            kUpgradingTo36,
            kFullyUpgradedTo36,
            kDowngradingTo34,
            kUnsetDefault34Behavior
        };

        /**
         * On startup, the featureCompatibilityVersion may not have been explicitly set yet. This
         * exposes the actual state of the featureCompatibilityVersion if it is uninitialized.
         */
        const bool isVersionInitialized() const {
            return _version.load() != Version::kUnsetDefault34Behavior;
        }

        /**
         * This safe getter for the featureCompatibilityVersion returns a default value when the
         * version has not yet been set.
         */
        const Version getVersion() const {
            Version v = _version.load();
            return (v == Version::kUnsetDefault34Behavior) ? Version::kFullyDowngradedTo34 : v;
        }

        void reset() {
            _version.store(Version::kFullyDowngradedTo34);
        }

        void setVersion(Version version) {
            return _version.store(version);
        }

        // This determines whether to give Collections UUIDs upon creation.
        const bool isSchemaVersion36() {
            return (getVersion() == Version::kFullyUpgradedTo36 ||
                    getVersion() == Version::kUpgradingTo36);
        }

    private:
        AtomicWord<Version> _version{Version::kUnsetDefault34Behavior};

    } featureCompatibility;

    // Feature validation differs depending on the role of a mongod in a replica set or
    // master/slave configuration. Masters/primaries can accept user-initiated writes and
    // validate based on the feature compatibility version. A secondary/slave (which is not also
    // a master) always validates in the upgraded mode so that it can sync new features, even
    // when in the downgraded feature compatibility mode.
    AtomicWord<bool> validateFeaturesAsMaster{true};

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
}
