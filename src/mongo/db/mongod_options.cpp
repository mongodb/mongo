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


#include "mongo/db/mongod_options.h"

#include <algorithm>
#include <boost/filesystem.hpp>  // IWYU pragma: keep
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/cluster_auth_mode_option_gen.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/keyfile_option_gen.h"
#include "mongo/db/mongod_options_general_gen.h"
#include "mongo/db/mongod_options_legacy_gen.h"
#include "mongo/db/mongod_options_replication_gen.h"
#include "mongo/db/mongod_options_sharding_gen.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config_params_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_nongeneral_gen.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/db/server_options_upgrade_downgrade_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

using std::endl;

std::string storageDBPathDescription() {
    StringBuilder sb;

    sb << "Directory for datafiles - defaults to " << storageGlobalParams.kDefaultDbPath;

#ifdef _WIN32
    boost::filesystem::path currentPath = boost::filesystem::current_path();

    sb << " which is " << currentPath.root_name().string() << storageGlobalParams.kDefaultDbPath
       << " based on the current working drive";
#endif

    return sb.str();
}

Status addMongodOptions(moe::OptionSection* options) try {
    uassertStatusOK(addGeneralServerOptions(options));
    uassertStatusOK(addNonGeneralServerOptions(options));
    uassertStatusOK(addMongodGeneralOptions(options));
    uassertStatusOK(addMongodReplicationOptions(options));
    uassertStatusOK(addMongodShardingOptions(options));
    uassertStatusOK(addMongodStorageOptions(options));
    uassertStatusOK(addMongodLegacyOptions(options));
    uassertStatusOK(addKeyfileServerOption(options));
    uassertStatusOK(addClusterAuthModeServerOption(options));
    uassertStatusOK(addServerUpgradeDowngradeOptions(options));

    return Status::OK();
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

void printMongodHelp(const moe::OptionSection& options) {
    std::cout << options.helpString() << std::endl;
};

namespace {

void appendSysInfo(BSONObjBuilder* obj) {
    auto o = BSONObjBuilder(obj->subobjStart("sysinfo"));
#if defined(_SC_PAGE_SIZE)
    o.append("_SC_PAGE_SIZE", (long long)sysconf(_SC_PAGE_SIZE));
#endif
#if defined(_SC_PHYS_PAGES)
    o.append("_SC_PHYS_PAGES", (long long)sysconf(_SC_PHYS_PAGES));
#endif
#if defined(_SC_AVPHYS_PAGES)
    o.append("_SC_AVPHYS_PAGES", (long long)sysconf(_SC_AVPHYS_PAGES));
#endif
}

StatusWith<repl::ReplSettings> populateReplSettings(const moe::Environment& params) {
    repl::ReplSettings replSettings;

    if (params.count("replication.serverless")) {
        if (params.count("replication.replSet") || params.count("replication.replSetName")) {
            return Status(ErrorCodes::BadValue,
                          "serverless cannot be used with replSet or replSetName options");
        }
        // Starting a node in "serverless" mode implies it uses a replSet.
        replSettings.setServerlessMode();
    } else if (params.count("replication.replSet")) {
        /* seed list of hosts for the repl set */
        replSettings.setReplSetString(params["replication.replSet"].as<std::string>().c_str());
    } else if (params.count("replication.replSetName")) {
        // "replSetName" is previously removed if "replSet" and "replSetName" are both found to be
        // set by the user. Therefore, we only need to check for it if "replSet" in not found.
        replSettings.setReplSetString(params["replication.replSetName"].as<std::string>().c_str());
    } else if (gFeatureFlagAllMongodsAreSharded.isEnabledUseLatestFCVWhenUninitialized(
                   serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
               serverGlobalParams.maintenanceMode != ServerGlobalParams::StandaloneMode) {
        replSettings.setShouldAutoInitiate();
        // Empty `replSet` in replSettings means that the replica set name will be auto-generated
        // in `processReplSetInitiate` after auto-initiation occurs or loaded from the
        // local replica set configuration document if already part of a replica set.
    }

    if (params.count("replication.oplogSizeMB")) {
        long long x = params["replication.oplogSizeMB"].as<int>();
        if (x <= 0) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "bad --oplogSize, arg must be greater than 0,"
                                           "found: "
                                        << x);
        }
        // note a small size such as x==1 is ok for an arbiter.
        if (x > 1000 && sizeof(void*) == 4) {
            StringBuilder sb;
            sb << "--oplogSize of " << x
               << "MB is too big for 32 bit version. Use 64 bit build instead.";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        replSettings.setOplogSizeBytes(x * 1024 * 1024);
        invariant(replSettings.getOplogSizeBytes() > 0);
    }

    return replSettings;
}

}  // namespace

bool handlePreValidationMongodOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args) {
    if (params.count("help") && params["help"].as<bool>() == true) {
        printMongodHelp(moe::startupOptions);
        return false;
    }
    if (params.count("version") && params["version"].as<bool>() == true) {
        auto&& vii = VersionInfoInterface::instance();
        std::cout << mongodVersion(vii) << std::endl;
        vii.logBuildInfo(&std::cout);
        return false;
    }
    if (params.count("sysinfo") && params["sysinfo"].as<bool>() == true) {
        BSONObjBuilder obj;
        appendSysInfo(&obj);
        std::cout << tojson(obj.done(), ExtendedRelaxedV2_0_0, true) << std::endl;
        return false;
    }

    if (params.count("master") || params.count("slave")) {
        LOGV2_FATAL_CONTINUE(20881, "Master/slave replication is no longer supported");
        return false;
    }

    if (params.count("replication.enableMajorityReadConcern") &&
        params["replication.enableMajorityReadConcern"].as<bool>() == false) {
        LOGV2_FATAL_CONTINUE(5324700, "enableMajorityReadConcern:false is no longer supported");
        return false;
    }

    return true;
}

Status validateMongodOptions(const moe::Environment& params) {
    Status ret = validateServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

#ifdef _WIN32
    if (params.count("install") || params.count("reinstall")) {
        if (params.count("storage.dbPath") &&
            !boost::filesystem::path(params["storage.dbPath"].as<std::string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "dbPath requires an absolute file path with Windows services");
        }
    }
#endif

    bool setConfigRole = params.count("configsvr");
    bool setShardRole = params.count("shardsvr");
    if (params.count("sharding.clusterRole")) {
        auto clusterRole = params["sharding.clusterRole"].as<std::string>();
        setConfigRole = setConfigRole || clusterRole == "configsvr";
        setShardRole = setShardRole || clusterRole == "shardsvr";
    }

    bool setRouterPort = params.count("routerPort") || params.count("net.routerPort");

    if (setRouterPort && !setConfigRole && !setShardRole) {
        return Status(ErrorCodes::BadValue,
                      "The embedded router requires the node to act as a shard or config server");
    }

    if (params.count("maintenanceMode")) {
        auto maintenanceMode = params["maintenanceMode"].as<std::string>();
        if (maintenanceMode == "standalone" &&
            (params.count("replSet") || params.count("replication.replSetName"))) {
            return Status(ErrorCodes::BadValue,
                          "Cannot specify both standalone maintenance mode and replica set name");
        }
    }

    if (params.count("storage.queryableBackupMode")) {
        // Command line options that are disallowed when --queryableBackupMode is specified.
        for (const auto& disallowedOption :
             {"replication.replSet", "configsvr", "upgrade", "repair", "profile", "restore"}) {
            if (params.count(disallowedOption)) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Cannot specify both queryable backup mode and "
                                            << disallowedOption);
            }
        }

        if (setShardRole && !params.count("sharding._overrideShardIdentity")) {
            return Status(
                ErrorCodes::BadValue,
                "shardsvr cluster role with queryableBackupMode requires _overrideShardIdentity");
        }
    }

    return Status::OK();
}

Status canonicalizeMongodOptions(moe::Environment* params) {
    Status ret = canonicalizeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    // "security.authorization" comes from the config file, so override it if "auth" is
    // set since those come from the command line.
    if (params->count("auth")) {
        Status ret =
            params->set("security.authorization",
                        (*params)["auth"].as<bool>() ? moe::Value(std::string("enabled"))
                                                     : moe::Value(std::string("disabled")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("auth");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // "sharding.clusterRole" comes from the config file, so override it if "configsvr" or
    // "shardsvr" are set since those come from the command line.
    if (params->count("configsvr")) {
        if ((*params)["configsvr"].as<bool>() == false) {
            // Handle the case where "configsvr" comes from the legacy config file and is set to
            // false.  This option is not allowed in the YAML config.
            return Status(ErrorCodes::BadValue,
                          "configsvr option cannot be set to false in config file");
        }
        Status ret = params->set("sharding.clusterRole", moe::Value(std::string("configsvr")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("configsvr");
        if (!ret.isOK()) {
            return ret;
        }
    }
    if (params->count("shardsvr")) {
        if ((*params)["shardsvr"].as<bool>() == false) {
            // Handle the case where "shardsvr" comes from the legacy config file and is set to
            // false.  This option is not allowed in the YAML config.
            return Status(ErrorCodes::BadValue,
                          "shardsvr option cannot be set to false in config file");
        }
        Status ret = params->set("sharding.clusterRole", moe::Value(std::string("shardsvr")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("shardsvr");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // If the "--routerPort" option is passed from the command line, override "net.routerPort"
    // (config file option) as it will be used later.
    if (params->count("routerPort")) {
        Status ret = params->set("net.routerPort", moe::Value((*params)["routerPort"].as<int>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("routerPort");
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (params->count("profile")) {
        int profilingMode;
        Status ret = params->get("profile", &profilingMode);
        if (!ret.isOK()) {
            return ret;
        }
        std::string profilingModeString;
        if (profilingMode == 0) {
            profilingModeString = "off";
        } else if (profilingMode == 1) {
            profilingModeString = "slowOp";
        } else if (profilingMode == 2) {
            profilingModeString = "all";
        } else {
            StringBuilder sb;
            sb << "Bad value for profile: " << profilingMode
               << ".  Supported modes are: (0=off|1=slowOp|2=all)";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        ret = params->set("operationProfiling.mode", moe::Value(profilingModeString));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("profile");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // Ensure that "replication.replSet" logically overrides "replication.replSetName".  We
    // can't canonicalize them as the same option, because they mean slightly different things.
    // "replication.replSet" can include a seed list, while "replication.replSetName" just has
    // the replica set name.
    if (params->count("replication.replSet") && params->count("replication.replSetName")) {
        ret = params->remove("replication.replSetName");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // "security.javascriptEnabled" comes from the config file, so override it if "noscripting"
    // is set since that comes from the command line.
    if (params->count("noscripting")) {
        Status ret = params->set("security.javascriptEnabled",
                                 moe::Value(!(*params)["noscripting"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("noscripting");
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

namespace {
constexpr char getPathSeparator() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

void removeTrailingPathSeparator(std::string& path, char separator) {
    while (path.size() > 1 && path.ends_with(separator)) {
        // size() check is for the unlikely possibility of --dbpath "/"
        path.pop_back();
    }
}
}  // namespace

Status storeMongodOptions(const moe::Environment& params) {
    Status ret = storeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    boost::optional<std::map<std::string, std::string>> setParameterMap;
    if (params.count("setParameter")) {
        setParameterMap.emplace(params["setParameter"].as<std::map<std::string, std::string>>());
    }

    auto checkConflictWithSetParameter = [&setParameterMap](const std::string& configName,
                                                            const std::string& parameterName) {
        if (setParameterMap && setParameterMap->find(parameterName) != setParameterMap->end()) {
            return Status(ErrorCodes::BadValue,
                          fmt::format("Conflicting server setting and setParameter, only one of "
                                      "the two should be used: config={}, setParameter={}",
                                      configName,
                                      parameterName));
        }
        return Status::OK();
    };

    // TODO: Integrate these options with their setParameter counterparts
    if (params.count("security.authSchemaVersion")) {
        return Status(ErrorCodes::BadValue,
                      "security.authSchemaVersion is currently not supported in config files");
    }

    if (params.count("security.enableLocalhostAuthBypass")) {
        return Status(ErrorCodes::BadValue,
                      "security.enableLocalhostAuthBypass is currently not supported in config "
                      "files");
    }

    if (params.count("storage.engine")) {
        storageGlobalParams.engine = params["storage.engine"].as<std::string>();
        storageGlobalParams.engineSetByUser = true;
    }

    if (params.count("storage.dbPath")) {
        storageGlobalParams.dbpath = params["storage.dbPath"].as<std::string>();
        if (params.count("processManagement.fork") && storageGlobalParams.dbpath[0] != '/') {
            // we need to change dbpath if we fork since we change
            // cwd to "/"
            // fork only exists on *nix
            // so '/' is safe
            storageGlobalParams.dbpath = serverGlobalParams.cwd + "/" + storageGlobalParams.dbpath;
        }
    }
    removeTrailingPathSeparator(storageGlobalParams.dbpath, getPathSeparator());

#ifdef _WIN32
    StringData dbpath(storageGlobalParams.dbpath);
    if (dbpath.size() >= 2 && dbpath.startsWith("\\\\")) {
        // Check if the dbpath is on a Windows network share (eg. \\myserver\myshare)
        LOGV2_WARNING_OPTIONS(5808500,
                              {logv2::LogTag::kStartupWarnings},
                              "dbpath should not be used on a network share");
    }
#endif

    if (params.count("operationProfiling.mode")) {
        std::string profilingMode = params["operationProfiling.mode"].as<std::string>();
        if (profilingMode == "off") {
            serverGlobalParams.defaultProfile = 0;
        } else if (profilingMode == "slowOp") {
            serverGlobalParams.defaultProfile = 1;
        } else if (profilingMode == "all") {
            serverGlobalParams.defaultProfile = 2;
        } else {
            StringBuilder sb;
            sb << "Bad value for operationProfiling.mode: " << profilingMode
               << ".  Supported modes are: (off|slowOp|all)";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    if (params.count("storage.syncPeriodSecs")) {
        storageGlobalParams.syncdelay = params["storage.syncPeriodSecs"].as<double>();
        Status conflictStatus =
            checkConflictWithSetParameter("storage.syncPeriodSecs", "syncdelay");
        if (!conflictStatus.isOK()) {
            return conflictStatus;
        }
    }

    if (params.count("storage.directoryPerDB")) {
        storageGlobalParams.directoryperdb = params["storage.directoryPerDB"].as<bool>();
    }

    if (params.count("storage.queryableBackupMode") &&
        params["storage.queryableBackupMode"].as<bool>()) {
        storageGlobalParams.queryableBackupMode = true;
    }

    if (params.count("storage.groupCollections")) {
        storageGlobalParams.groupCollections = params["storage.groupCollections"].as<bool>();
    }

    if (params.count("storage.journal.commitIntervalMs")) {
        auto journalCommitIntervalMs = params["storage.journal.commitIntervalMs"].as<int>();
        storageGlobalParams.journalCommitIntervalMs.store(journalCommitIntervalMs);
        Status conflictStatus = checkConflictWithSetParameter("storage.journal.commitIntervalMs",
                                                              "journalCommitInterval");
        if (!conflictStatus.isOK()) {
            return conflictStatus;
        }
    }

    if (params.count("security.javascriptEnabled")) {
        mongodGlobalParams.scriptingEnabled = params["security.javascriptEnabled"].as<bool>();
    }

    if (params.count("security.clusterIpSourceAllowlist")) {
        auto allowlistedClusterNetwork = std::make_shared<std::vector<std::string>>();
        for (const std::string& allowlistEntry :
             params["security.clusterIpSourceAllowlist"].as<std::vector<std::string>>()) {
            std::vector<std::string> intermediates;
            str::splitStringDelim(allowlistEntry, &intermediates, ',');
            std::copy(intermediates.begin(),
                      intermediates.end(),
                      std::back_inserter(*allowlistedClusterNetwork));
        }
        mongodGlobalParams.allowlistedClusterNetwork = allowlistedClusterNetwork;
    }

    if (params.count("repair") && params["repair"].as<bool>() == true) {
        storageGlobalParams.upgrade = 1;  // --repair implies --upgrade
        storageGlobalParams.repair = 1;
    }
    if (params.count("upgrade") && params["upgrade"].as<bool>() == true) {
        storageGlobalParams.upgrade = 1;
    }
    if (params.count("notablescan")) {
        storageGlobalParams.noTableScan.store(params["notablescan"].as<bool>());
    }
    if (params.count("restore") && params["restore"].as<bool>() == true) {
        storageGlobalParams.restore = 1;

        if (storageGlobalParams.repair) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Cannot specify both --repair and --restore");
        }
    }
    if (params.count("magicRestore") && params["magicRestore"].as<bool>() == true) {
        storageGlobalParams.magicRestore = 1;

        // Use an ephemeral port so that users don't connect to a node that is being restored.
        if (!params.count("net.port")) {
            serverGlobalParams.port = ServerGlobalParams::DefaultMagicRestorePort;
        }
    }

    if (params.count("maintenanceMode") &&
        gFeatureFlagAllMongodsAreSharded.isEnabledUseLatestFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // Setting maintenanceMode will disable sharding by setting 'clusterRole' to
        // 'ClusterRole::None'. If maintenanceMode is set to 'standalone', replication will be
        // disabled as well.
        std::string value = params["maintenanceMode"].as<std::string>();
        serverGlobalParams.maintenanceMode = (value == "replicaSet")
            ? ServerGlobalParams::ReplicaSetMode
            : ServerGlobalParams::StandaloneMode;
    }

    const auto replSettingsWithStatus = populateReplSettings(params);
    if (!replSettingsWithStatus.isOK())
        return replSettingsWithStatus.getStatus();
    const repl::ReplSettings& replSettings(replSettingsWithStatus.getValue());

    if (replSettings.isReplSet()) {
        if ((params.count("security.authorization") &&
             params["security.authorization"].as<std::string>() == "enabled") &&
            !serverGlobalParams.startupClusterAuthMode.x509Only() &&
            serverGlobalParams.keyFile.empty()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "security.keyFile is required when authorization is "
                                           "enabled with replica sets");
        }
    } else {
        // If we are not using a replica set, then we are in standalone mode.
        //
        // A standalone node does not use the oplog collection, so special truncation handling for
        // the capped collection is unnecessary.
        //
        // A standalone node that will be reintroduced to its replica set must not allow oplog
        // truncation while in standalone mode because oplog history needed for startup recovery as
        // a replica set member could be deleted. Replication can need history older than the last
        // checkpoint to support transactions.
        //
        // Note: we only use this to defer oplog collection truncation via OplogTruncateMarkers in
        // WT. Non-WT storage engines will continue to perform regular capped collection handling
        // for the oplog collection, regardless of this parameter setting.
        storageGlobalParams.allowOplogTruncation = false;
    }

    serverGlobalParams.enableMajorityReadConcern = true;

    if (storageGlobalParams.engineSetByUser && (storageGlobalParams.engine == "devnull")) {
        LOGV2(5324701,
              "Test storage engine does not support enableMajorityReadConcern=true, forcibly "
              "setting to false",
              "storageEngine"_attr = storageGlobalParams.engine);
        serverGlobalParams.enableMajorityReadConcern = false;
    }

    if (!serverGlobalParams.enableMajorityReadConcern) {
        // Lock-free reads is not supported with enableMajorityReadConcern=false, so we disable it.
        if (!storageGlobalParams.disableLockFreeReads) {
            LOGV2_WARNING(4788401,
                          "Lock-free reads is not compatible with "
                          "enableMajorityReadConcern=false: disabling lock-free reads.");
            storageGlobalParams.disableLockFreeReads = true;
        }
    }

    if (params.count("storage.oplogMinRetentionHours")) {
        storageGlobalParams.oplogMinRetentionHours.store(
            params["storage.oplogMinRetentionHours"].as<double>());
        if (storageGlobalParams.oplogMinRetentionHours.load() < 0) {
            return Status(ErrorCodes::BadValue,
                          "bad --oplogMinRetentionHours, argument must be greater or equal to 0");
        }
        invariant(storageGlobalParams.oplogMinRetentionHours.load() >= 0);
    }

    if (params.count("cacheSize")) {
        long x = params["cacheSize"].as<long>();
        if (x <= 0) {
            return Status(ErrorCodes::BadValue, "bad --cacheSize arg");
        }
        return Status(ErrorCodes::BadValue, "--cacheSize option not currently supported");
    }
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (params.count("sharding.clusterRole")) {
        auto clusterRoleParam = params["sharding.clusterRole"].as<std::string>();
        // Force to set up the node as a replica set, unless we're a shard and we're using queryable
        // backup mode.
        if ((clusterRoleParam == "configsvr" || !params.count("storage.queryableBackupMode")) &&
            !replSettings.isReplSet()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Cannot start a " << clusterRoleParam
                                        << " as a standalone server. Please use the option "
                                           "--replSet to start the node as a replica set.");
        }
        if (clusterRoleParam == "configsvr") {
            serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
            // Config server requires majority read concern.
            uassert(5324702,
                    str::stream() << "Cannot initialize config server with "
                                  << "enableMajorityReadConcern=false",
                    serverGlobalParams.enableMajorityReadConcern);

            if (!params.count("storage.dbPath")) {
                storageGlobalParams.dbpath = storageGlobalParams.kDefaultConfigDbPath;
            }
        } else if (clusterRoleParam == "shardsvr") {
            serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        } else {
            return Status(ErrorCodes::BadValue,
                          fmt::format("Bad value for sharding.clusterRole: {}. Supported modes "
                                      "are: (configsvr|shardsvr)",
                                      clusterRoleParam));
        }

        // Every node in a sharded cluster will have by default the RouterServer role. As a
        // consequence, the only possible combinations are:
        // - { ShardServer, RouterServer }
        // - { ShardServer, ConfigServer, RouterServer }
        // - { RouterServer }
        serverGlobalParams.clusterRole += ClusterRole::RouterServer;

        if (params.count("net.routerPort")) {
            if (feature_flags::gEmbeddedRouter.isEnabledUseLatestFCVWhenUninitialized(
                    fcvSnapshot)) {
                serverGlobalParams.routerPort = params["net.routerPort"].as<int>();
            }
        }
    } else if (gFeatureFlagAllMongodsAreSharded.isEnabledUseLatestFCVWhenUninitialized(
                   fcvSnapshot) &&
               serverGlobalParams.maintenanceMode == ServerGlobalParams::MaintenanceMode::None) {
        serverGlobalParams.doAutoBootstrapSharding = true;
        serverGlobalParams.clusterRole = {
            ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};
    }

    if (!params.count("net.port")) {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            serverGlobalParams.port = ServerGlobalParams::ConfigServerPort;
        } else if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            serverGlobalParams.port = ServerGlobalParams::ShardServerPort;
        }
    }

    if (params.count("sharding._overrideShardIdentity")) {
        auto docAsString = params["sharding._overrideShardIdentity"].as<std::string>();

        try {
            serverGlobalParams.overrideShardIdentity = fromjson(docAsString);
        } catch (const DBException& exception) {
            return exception.toStatus(
                "Error encountered while parsing _overrideShardIdentity JSON document");
        }
    }

    if (params.count("pairwith") || params.count("arbiter") || params.count("opIdMem")) {
        return Status(ErrorCodes::BadValue,
                      "****\n"
                      "Replica Pairs have been deprecated. Invalid options: "
                      "--pairwith, --arbiter, and/or --opIdMem\n"
                      "<http://dochub.mongodb.org/core/replicapairs>\n"
                      "****");
    }

#ifdef _WIN32
    // If dbPath is a default value, prepend with drive name so log entries are explicit
    // We must resolve the dbpath before it stored in repairPath in the default case.
    if (storageGlobalParams.dbpath == storageGlobalParams.kDefaultDbPath ||
        storageGlobalParams.dbpath == storageGlobalParams.kDefaultConfigDbPath) {
        boost::filesystem::path currentPath = boost::filesystem::current_path();
        storageGlobalParams.dbpath = currentPath.root_name().string() + storageGlobalParams.dbpath;
    }
#endif

    bool isClusterRoleShard = params.count("shardsvr");
    bool isClusterRoleConfig = params.count("configsvr");
    if (params.count("sharding.clusterRole")) {
        auto clusterRole = params["sharding.clusterRole"].as<std::string>();
        isClusterRoleShard = isClusterRoleShard || (clusterRole == "shardsvr");
        isClusterRoleConfig = isClusterRoleConfig || (clusterRole == "configsvr");
    }

    if ((isClusterRoleShard || isClusterRoleConfig) && skipShardingConfigurationChecks) {
        auto clusterRoleStr = isClusterRoleConfig ? "--configsvr" : "--shardsvr";
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Can not specify " << clusterRoleStr
                                    << " and set skipShardingConfigurationChecks=true");
    }

    if ((isClusterRoleShard || isClusterRoleConfig) && params.count("setParameter")) {
        std::map<std::string, std::string> parameters =
            params["setParameter"].as<std::map<std::string, std::string>>();
        const bool requireApiVersionValue = ([&parameters] {
            const auto requireApiVersionParam = parameters.find("requireApiVersion");
            if (requireApiVersionParam == parameters.end()) {
                return false;
            }
            const auto& val = requireApiVersionParam->second;
            return (0 == val.compare("1")) || (0 == val.compare("true"));
        })();

        if (requireApiVersionValue) {
            auto clusterRoleStr = isClusterRoleConfig ? "--configsvr" : "--shardsvr";
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Can not specify " << clusterRoleStr
                                        << " and set requireApiVersion=true");
        }
    }

    serverGlobalParams.upgradeBackCompat = params.count("upgradeBackCompat");
    serverGlobalParams.downgradeBackCompat = params.count("downgradeBackCompat");

    setGlobalReplSettings(replSettings);
    return Status::OK();
}

namespace {
std::function<ExitCode(ServiceContext* svcCtx)> _magicRestoreMainFn = nullptr;
}

void setMagicRestoreMain(std::function<ExitCode(ServiceContext* svcCtx)> magicRestoreMainFn) {
    _magicRestoreMainFn = magicRestoreMainFn;
}
std::function<ExitCode(ServiceContext* svcCtx)> getMagicRestoreMain() {
    return _magicRestoreMainFn;
}

}  // namespace mongo
