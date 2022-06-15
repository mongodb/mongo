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

#include <boost/filesystem.hpp>
#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/cluster_auth_mode_option_gen.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/keyfile_option_gen.h"
#include "mongo/db/mongod_options_general_gen.h"
#include "mongo/db/mongod_options_legacy_gen.h"
#include "mongo/db/mongod_options_replication_gen.h"
#include "mongo/db/mongod_options_sharding_gen.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_nongeneral_gen.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

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

        bool isClusterRoleShard = params.count("shardsvr");
        if (params.count("sharding.clusterRole")) {
            auto clusterRole = params["sharding.clusterRole"].as<std::string>();
            isClusterRoleShard = isClusterRoleShard || (clusterRole == "shardsvr");
        }

        if (isClusterRoleShard && !params.count("sharding._overrideShardIdentity")) {
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

    // "sharding.archiveMovedChunks" comes from the config file, so override it if
    // "noMoveParanoia" or "moveParanoia" are set since those come from the command line.
    if (params->count("noMoveParanoia")) {
        Status ret = params->set("sharding.archiveMovedChunks",
                                 moe::Value(!(*params)["noMoveParanoia"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("noMoveParanoia");
        if (!ret.isOK()) {
            return ret;
        }
    }
    if (params->count("moveParanoia")) {
        Status ret = params->set("sharding.archiveMovedChunks",
                                 moe::Value((*params)["moveParanoia"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("moveParanoia");
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

Status storeMongodOptions(const moe::Environment& params) {
    Status ret = storeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

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
#ifdef _WIN32
    if (storageGlobalParams.dbpath.size() > 1 &&
        storageGlobalParams.dbpath[storageGlobalParams.dbpath.size() - 1] == '/') {
        // size() check is for the unlikely possibility of --dbpath "/"
        storageGlobalParams.dbpath =
            storageGlobalParams.dbpath.erase(storageGlobalParams.dbpath.size() - 1);
    }

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
        storageGlobalParams.checkpointDelaySecs =
            static_cast<size_t>(params["storage.syncPeriodSecs"].as<double>());

        if (storageGlobalParams.syncdelay < 0 ||
            storageGlobalParams.syncdelay > StorageGlobalParams::kMaxSyncdelaySecs) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "syncdelay out of allowed range (0-"
                                        << StorageGlobalParams::kMaxSyncdelaySecs << "s)");
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
        if (journalCommitIntervalMs < 1 ||
            journalCommitIntervalMs > StorageGlobalParams::kMaxJournalCommitIntervalMs) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "--journalCommitInterval out of allowed range (1-"
                              << StorageGlobalParams::kMaxJournalCommitIntervalMs << "ms)");
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

    repl::ReplSettings replSettings;
    if (params.count("replication.serverless")) {
        if (params.count("replication.replSet") || params.count("replication.replSetName")) {
            return Status(ErrorCodes::BadValue,
                          "serverless cannot be used with replSet or replSetName options");
        }
        // Starting a node in "serverless" mode implies it uses a replSet.
        replSettings.setServerlessMode();
    }
    if (params.count("replication.replSet")) {
        /* seed list of hosts for the repl set */
        replSettings.setReplSetString(params["replication.replSet"].as<std::string>().c_str());
    } else if (params.count("replication.replSetName")) {
        // "replSetName" is previously removed if "replSet" and "replSetName" are both found to be
        // set by the user. Therefore, we only need to check for it if "replSet" in not found.
        replSettings.setReplSetString(params["replication.replSetName"].as<std::string>().c_str());
    } else {
        // If neither "replication.replSet" nor "replication.replSetName" is set, then we are in
        // standalone mode.
        //
        // A standalone node does not use the oplog collection, so special truncation handling for
        // the capped collection is unnecessary.
        //
        // A standalone node that will be reintroduced to its replica set must not allow oplog
        // truncation while in standalone mode because oplog history needed for startup recovery as
        // a replica set member could be deleted. Replication can need history older than the last
        // checkpoint to support transactions.
        //
        // Note: we only use this to defer oplog collection truncation via OplogStones in WT. Non-WT
        // storage engines will continue to perform regular capped collection handling for the oplog
        // collection, regardless of this parameter setting.
        storageGlobalParams.allowOplogTruncation = false;
    }

    if (replSettings.usingReplSets() &&
        (params.count("security.authorization") &&
         params["security.authorization"].as<std::string>() == "enabled") &&
        !serverGlobalParams.startupClusterAuthMode.x509Only() &&
        serverGlobalParams.keyFile.empty()) {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "security.keyFile is required when authorization is enabled with replica sets");
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
    if (!params.count("net.port")) {
        if (params.count("sharding.clusterRole")) {
            std::string clusterRole = params["sharding.clusterRole"].as<std::string>();
            if (clusterRole == "configsvr") {
                serverGlobalParams.port = ServerGlobalParams::ConfigServerPort;
            } else if (clusterRole == "shardsvr") {
                serverGlobalParams.port = ServerGlobalParams::ShardServerPort;
            } else {
                StringBuilder sb;
                sb << "Bad value for sharding.clusterRole: " << clusterRole
                   << ".  Supported modes are: (configsvr|shardsvr)";
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
    } else {
        if (serverGlobalParams.port < 0 || serverGlobalParams.port > 65535) {
            return Status(ErrorCodes::BadValue, "bad --port number");
        }
    }
    if (params.count("sharding.clusterRole")) {
        auto clusterRoleParam = params["sharding.clusterRole"].as<std::string>();
        const bool replicationEnabled = params.count("replication.replSet") ||
            params.count("replication.replSetName") || params.count("replication.serverless");
        // Force to set up the node as a replica set, unless we're a shard and we're using queryable
        // backup mode.
        if ((clusterRoleParam == "configsvr" || !params.count("storage.queryableBackupMode")) &&
            !replicationEnabled) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Cannot start a " << clusterRoleParam
                                        << " as a standalone server. Please use the option "
                                           "--replSet to start the node as a replica set.");
        }
        if (clusterRoleParam == "configsvr") {
            serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
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
        }
    }

    if (params.count("sharding.archiveMovedChunks")) {
        serverGlobalParams.moveParanoia = params["sharding.archiveMovedChunks"].as<bool>();
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

    setGlobalReplSettings(replSettings);
    return Status::OK();
}

}  // namespace mongo
