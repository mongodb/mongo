/*
 *    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/db/mongod_options.h"

#include <boost/filesystem.hpp>
#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/db.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/version.h"

namespace mongo {

using std::cout;
using std::endl;
using std::string;

MongodGlobalParams mongodGlobalParams;

extern DiagLog _diaglog;

Status addMongodOptions(moe::OptionSection* options) {
    moe::OptionSection general_options("General options");

    Status ret = addGeneralServerOptions(&general_options);
    if (!ret.isOK()) {
        return ret;
    }

#if defined(_WIN32)
    moe::OptionSection windows_scm_options("Windows Service Control Manager options");

    ret = addWindowsServerOptions(&windows_scm_options);
    if (!ret.isOK()) {
        return ret;
    }
#endif

#ifdef MONGO_CONFIG_SSL
    moe::OptionSection ssl_options("SSL options");

    ret = addSSLServerOptions(&ssl_options);
    if (!ret.isOK()) {
        return ret;
    }
#endif

    moe::OptionSection ms_options("Master/slave options (old; use replica sets instead)");
    moe::OptionSection rs_options("Replica set options");
    moe::OptionSection replication_options("Replication options");
    moe::OptionSection sharding_options("Sharding options");
    moe::OptionSection storage_options("Storage options");

    // Authentication Options

    // Way to enable or disable auth on command line and in Legacy config file
    general_options.addOptionChaining("auth", "auth", moe::Switch, "run with security")
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("noauth");

    // Way to enable or disable auth in JSON Config
    general_options
        .addOptionChaining(
            "security.authorization",
            "",
            moe::String,
            "How the database behaves with respect to authorization of clients.  "
            "Options are \"disabled\", which means that authorization checks are not "
            "performed, and \"enabled\" which means that a client cannot perform actions it is "
            "not authorized to do.")
        .setSources(moe::SourceYAMLConfig)
        .format("(:?disabled)|(:?enabled)", "(disabled/enabled)");

    // setParameter parameters that we want as config file options
    // TODO: Actually read these into our environment.  Currently they have no effect
    general_options.addOptionChaining("security.authSchemaVersion", "", moe::String, "TODO")
        .setSources(moe::SourceYAMLConfig);

    general_options.addOptionChaining("security.enableLocalhostAuthBypass", "", moe::String, "TODO")
        .setSources(moe::SourceYAMLConfig);


    // Network Options

    general_options.addOptionChaining("net.http.JSONPEnabled",
                                      "jsonp",
                                      moe::Switch,
                                      "allow JSONP access via http (has security implications)");

    general_options.addOptionChaining(
        "net.http.RESTInterfaceEnabled", "rest", moe::Switch, "turn on simple rest api");

    // Diagnostic Options

    general_options
        .addOptionChaining(
            "diaglog", "diaglog", moe::Int, "DEPRECATED: 0=off 1=W 2=R 3=both 7=W+some reads")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    general_options
        .addOptionChaining("operationProfiling.slowOpThresholdMs",
                           "slowms",
                           moe::Int,
                           "value of slow for profile and console log")
        .setDefault(moe::Value(100));

    general_options.addOptionChaining("profile", "profile", moe::Int, "0=off 1=slow, 2=all")
        .setSources(moe::SourceAllLegacy);

    general_options
        .addOptionChaining("operationProfiling.mode", "", moe::String, "(off/slowOp/all)")
        .setSources(moe::SourceYAMLConfig)
        .format("(:?off)|(:?slowOp)|(:?all)", "(off/slowOp/all)");

    general_options
        .addOptionChaining(
            "cpu", "cpu", moe::Switch, "periodically show cpu and iowait utilization")
        .setSources(moe::SourceAllLegacy);

    general_options
        .addOptionChaining(
            "sysinfo", "sysinfo", moe::Switch, "print some diagnostic system information")
        .setSources(moe::SourceAllLegacy);

    // Storage Options

    storage_options.addOptionChaining(
        "storage.engine",
        "storageEngine",
        moe::String,
        "what storage engine to use - defaults to wiredTiger if no data files present");


#ifdef _WIN32
    boost::filesystem::path currentPath = boost::filesystem::current_path();

    std::string defaultPath = currentPath.root_name().string() + storageGlobalParams.kDefaultDbPath;
    storage_options.addOptionChaining("storage.dbPath",
                                      "dbpath",
                                      moe::String,
                                      std::string("directory for datafiles - defaults to ") +
                                          storageGlobalParams.kDefaultDbPath + " which is " +
                                          defaultPath + " based on the current working drive");

#else
    storage_options.addOptionChaining("storage.dbPath",
                                      "dbpath",
                                      moe::String,
                                      std::string("directory for datafiles - defaults to ") +
                                          storageGlobalParams.kDefaultDbPath);

#endif
    storage_options.addOptionChaining("storage.directoryPerDB",
                                      "directoryperdb",
                                      moe::Switch,
                                      "each database will be stored in a separate directory");

    storage_options
        .addOptionChaining("storage.queryableBackupMode",
                           "queryableBackupMode",
                           moe::Switch,
                           "enable read-only mode - if true the server will not accept writes.")
        .setSources(moe::SourceAll)
        .hidden();


    general_options
        .addOptionChaining("noIndexBuildRetry",
                           "noIndexBuildRetry",
                           moe::Switch,
                           "don't retry any index builds that were interrupted by shutdown")
        .setSources(moe::SourceAllLegacy);

    general_options
        .addOptionChaining("storage.indexBuildRetry",
                           "",
                           moe::Bool,
                           "don't retry any index builds that were interrupted by shutdown")
        .setSources(moe::SourceYAMLConfig);

    storage_options
        .addOptionChaining("noprealloc",
                           "noprealloc",
                           moe::Switch,
                           "disable data file preallocation - will often hurt performance")
        .setSources(moe::SourceAllLegacy);

    storage_options
        .addOptionChaining("storage.mmapv1.preallocDataFiles",
                           "",
                           moe::Bool,
                           "disable data file preallocation - will often hurt performance",
                           "storage.preallocDataFiles")
        .setSources(moe::SourceYAMLConfig);

    storage_options
        .addOptionChaining("storage.mmapv1.nsSize",
                           "nssize",
                           moe::Int,
                           ".ns file size (in MB) for new databases",
                           "storage.nsSize")
        .setDefault(moe::Value(16));

    storage_options
        .addOptionChaining("storage.mmapv1.quota.enforced",
                           "quota",
                           moe::Switch,
                           "limits each database to a certain number of files (8 default)",
                           "storage.quota.enforced")
        .incompatibleWith("keyFile");

    storage_options.addOptionChaining("storage.mmapv1.quota.maxFilesPerDB",
                                      "quotaFiles",
                                      moe::Int,
                                      "number of files allowed per db, implies --quota",
                                      "storage.quota.maxFilesPerDB");

    storage_options.addOptionChaining("storage.mmapv1.smallFiles",
                                      "smallfiles",
                                      moe::Switch,
                                      "use a smaller default file size",
                                      "storage.smallFiles");

    storage_options
        .addOptionChaining("storage.syncPeriodSecs",
                           "syncdelay",
                           moe::Double,
                           "seconds between disk syncs (0=never, but not recommended)")
        .setDefault(moe::Value(60.0));

    // Upgrade and repair are disallowed in JSON configs since they trigger very heavyweight
    // actions rather than specify configuration data
    storage_options.addOptionChaining("upgrade", "upgrade", moe::Switch, "upgrade db if needed")
        .setSources(moe::SourceAllLegacy);

    storage_options.addOptionChaining("repair", "repair", moe::Switch, "run repair on all dbs")
        .setSources(moe::SourceAllLegacy);

    storage_options.addOptionChaining("storage.repairPath",
                                      "repairpath",
                                      moe::String,
                                      "root directory for repair files - defaults to dbpath");

    // Javascript Options

    general_options
        .addOptionChaining("noscripting", "noscripting", moe::Switch, "disable scripting engine")
        .setSources(moe::SourceAllLegacy);

    general_options
        .addOptionChaining(
            "security.javascriptEnabled", "", moe::Bool, "Enable javascript execution")
        .setSources(moe::SourceYAMLConfig);

    // Query Options

    general_options
        .addOptionChaining("notablescan", "notablescan", moe::Switch, "do not allow table scans")
        .setSources(moe::SourceAllLegacy);

    // Journaling Options

    // Way to enable or disable journaling on command line and in Legacy config file
    storage_options.addOptionChaining("journal", "journal", moe::Switch, "enable journaling")
        .setSources(moe::SourceAllLegacy);

    storage_options
        .addOptionChaining("nojournal",
                           "nojournal",
                           moe::Switch,
                           "disable journaling (journaling is on by default for 64 bit)")
        .setSources(moe::SourceAllLegacy);

    storage_options.addOptionChaining("dur", "dur", moe::Switch, "enable journaling")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    storage_options.addOptionChaining("nodur", "nodur", moe::Switch, "disable journaling")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    // Way to enable or disable journaling in JSON Config
    general_options.addOptionChaining("storage.journal.enabled", "", moe::Bool, "enable journaling")
        .setSources(moe::SourceYAMLConfig);

    // Two ways to set durability diagnostic options.  durOptions is deprecated
    storage_options
        .addOptionChaining("storage.mmapv1.journal.debugFlags",
                           "journalOptions",
                           moe::Int,
                           "journal diagnostic options",
                           "storage.journal.debugFlags")
        .incompatibleWith("durOptions");

    storage_options
        .addOptionChaining("durOptions", "durOptions", moe::Int, "durability diagnostic options")
        .hidden()
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("storage.mmapv1.journal.debugFlags");

    storage_options.addOptionChaining("storage.journal.commitIntervalMs",
                                      "journalCommitInterval",
                                      moe::Int,
                                      "how often to group/batch commit (ms)",
                                      "storage.mmapv1.journal.commitIntervalMs");

    // Deprecated option that we don't want people to use for performance reasons
    storage_options
        .addOptionChaining(
            "nopreallocj", "nopreallocj", moe::Switch, "don't preallocate journal files")
        .hidden()
        .setSources(moe::SourceAllLegacy);

#if defined(__linux__)
    general_options.addOptionChaining(
        "shutdown", "shutdown", moe::Switch, "kill a running server (for init scripts)");

#endif

    // Master Slave Options

    ms_options.addOptionChaining("master", "master", moe::Switch, "master mode")
        .incompatibleWith("replication.replSet")
        .incompatibleWith("replication.replSetName")
        .setSources(moe::SourceAllLegacy);

    ms_options.addOptionChaining("slave", "slave", moe::Switch, "slave mode")
        .incompatibleWith("replication.replSet")
        .incompatibleWith("replication.replSetName")
        .setSources(moe::SourceAllLegacy);

    ms_options
        .addOptionChaining(
            "source", "source", moe::String, "when slave: specify master as <server:port>")
        .incompatibleWith("replication.replSet")
        .incompatibleWith("replication.replSetName")
        .setSources(moe::SourceAllLegacy);

    ms_options
        .addOptionChaining(
            "only", "only", moe::String, "when slave: specify a single database to replicate")
        .incompatibleWith("replication.replSet")
        .incompatibleWith("replication.replSetName")
        .setSources(moe::SourceAllLegacy);

    ms_options
        .addOptionChaining(
            "slavedelay",
            "slavedelay",
            moe::Int,
            "specify delay (in seconds) to be used when applying master ops to slave")
        .incompatibleWith("replication.replSet")
        .incompatibleWith("replication.replSetName")
        .setSources(moe::SourceAllLegacy);

    ms_options
        .addOptionChaining(
            "autoresync", "autoresync", moe::Switch, "automatically resync if slave data is stale")
        .incompatibleWith("replication.replSet")
        .incompatibleWith("replication.replSetName")
        .setSources(moe::SourceAllLegacy);

    // Replication Options

    replication_options.addOptionChaining(
        "replication.oplogSizeMB",
        "oplogSize",
        moe::Int,
        "size to use (in MB) for replication op log. default is 5% of disk space "
        "(i.e. large is good)");

    rs_options
        .addOptionChaining("replication.replSet",
                           "replSet",
                           moe::String,
                           "arg is <setname>[/<optionalseedhostlist>]")
        .setSources(moe::SourceAllLegacy);

    rs_options.addOptionChaining("replication.replSetName", "", moe::String, "arg is <setname>")
        .setSources(moe::SourceYAMLConfig)
        .format("[^/]+", "[replica set name with no \"/\"]");

    rs_options
        .addOptionChaining("replication.secondaryIndexPrefetch",
                           "replIndexPrefetch",
                           moe::String,
                           "specify index prefetching behavior (if secondary) [none|_id_only|all]")
        .format("(:?none)|(:?_id_only)|(:?all)", "(none/_id_only/all)");

    rs_options.addOptionChaining("replication.enableMajorityReadConcern",
                                 "enableMajorityReadConcern",
                                 moe::Switch,
                                 "enables majority readConcern");

    // Sharding Options

    sharding_options
        .addOptionChaining("configsvr",
                           "configsvr",
                           moe::Switch,
                           "declare this is a config db of a cluster; default port 27019; "
                           "default dir /data/configdb")
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("shardsvr");

    sharding_options
        .addOptionChaining("shardsvr",
                           "shardsvr",
                           moe::Switch,
                           "declare this is a shard db of a cluster; default port 27018")
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("configsvr");

    sharding_options
        .addOptionChaining(
            "sharding.clusterRole",
            "",
            moe::String,
            "Choose what role this mongod has in a sharded cluster.  Possible values are:\n"
            "    \"configsvr\": Start this node as a config server.  Starts on port 27019 by "
            "default."
            "    \"shardsvr\": Start this node as a shard server.  Starts on port 27018 by "
            "default.")
        .setSources(moe::SourceYAMLConfig)
        .format("(:?configsvr)|(:?shardsvr)", "(configsvr/shardsvr)");

    sharding_options
        .addOptionChaining(
            "sharding._overrideShardIdentity",
            "",
            moe::String,
            "overrides the shardIdentity document settings stored in the local storage with "
            "a MongoDB Extended JSON document in string format")
        .setSources(moe::SourceYAMLConfig)
        .incompatibleWith("configsvr")
        .requires("storage.queryableBackupMode");

    sharding_options
        .addOptionChaining("noMoveParanoia",
                           "noMoveParanoia",
                           moe::Switch,
                           "turn off paranoid saving of data for the moveChunk command; default")
        .hidden()
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("moveParanoia");

    sharding_options
        .addOptionChaining("moveParanoia",
                           "moveParanoia",
                           moe::Switch,
                           "turn on paranoid saving of data during the moveChunk command "
                           "(used for internal system diagnostics)")
        .hidden()
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("noMoveParanoia");

    sharding_options
        .addOptionChaining("sharding.archiveMovedChunks",
                           "",
                           moe::Bool,
                           "config file option to turn on paranoid saving of data during the "
                           "moveChunk command (used for internal system diagnostics)")
        .hidden()
        .setSources(moe::SourceYAMLConfig);


    options->addSection(general_options);
#if defined(_WIN32)
    options->addSection(windows_scm_options);
#endif
    options->addSection(replication_options);
    options->addSection(ms_options);
    options->addSection(rs_options);
    options->addSection(sharding_options);
#ifdef MONGO_CONFIG_SSL
    options->addSection(ssl_options);
#endif
    options->addSection(storage_options);

    // The following are legacy options that are disallowed in the JSON config file

    options
        ->addOptionChaining(
            "fastsync",
            "fastsync",
            moe::Switch,
            "indicate that this instance is starting from a dbpath snapshot of the repl peer")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    options
        ->addOptionChaining("pretouch",
                            "pretouch",
                            moe::Int,
                            "n pretouch threads for applying master/slave operations")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    // This is a deprecated option that we are supporting for backwards compatibility
    // The first value for this option can be either 'dbpath' or 'run'.
    // If it is 'dbpath', mongod prints the dbpath and exits.  Any extra values are ignored.
    // If it is 'run', mongod runs normally.  Providing extra values is an error.
    options->addOptionChaining("command", "command", moe::StringVector, "command")
        .hidden()
        .positional(1, 3)
        .setSources(moe::SourceAllLegacy);

    options
        ->addOptionChaining("cacheSize", "cacheSize", moe::Long, "cache size (in MB) for rec store")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    // deprecated pairing command line options
    options->addOptionChaining("pairwith", "pairwith", moe::Switch, "DEPRECATED")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    options->addOptionChaining("arbiter", "arbiter", moe::Switch, "DEPRECATED")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    options->addOptionChaining("opIdMem", "opIdMem", moe::Switch, "DEPRECATED")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    return Status::OK();
}

void printMongodHelp(const moe::OptionSection& options) {
    std::cout << options.helpString() << std::endl;
};

namespace {
void sysRuntimeInfo() {
#if defined(_SC_PAGE_SIZE)
    log() << "  page size: " << (int)sysconf(_SC_PAGE_SIZE) << endl;
#endif
#if defined(_SC_PHYS_PAGES)
    log() << "  _SC_PHYS_PAGES: " << sysconf(_SC_PHYS_PAGES) << endl;
#endif
#if defined(_SC_AVPHYS_PAGES)
    log() << "  _SC_AVPHYS_PAGES: " << sysconf(_SC_AVPHYS_PAGES) << endl;
#endif
}
}  // namespace

void setPlainConsoleLogger() {
    logger::LogManager* manager = logger::globalLogManager();
    manager->getGlobalDomain()->clearAppenders();
    manager->getGlobalDomain()->attachAppender(logger::MessageLogDomain::AppenderAutoPtr(
        new logger::ConsoleAppender<logger::MessageEventEphemeral>(
            new logger::MessageEventUnadornedEncoder)));
}

bool handlePreValidationMongodOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args) {
    if (params.count("help") && params["help"].as<bool>() == true) {
        printMongodHelp(moe::startupOptions);
        return false;
    }
    if (params.count("version") && params["version"].as<bool>() == true) {
        setPlainConsoleLogger();
        log() << mongodVersion() << endl;
        printBuildInfo();
        return false;
    }
    if (params.count("sysinfo") && params["sysinfo"].as<bool>() == true) {
        setPlainConsoleLogger();
        sysRuntimeInfo();
        return false;
    }

    return true;
}

Status validateMongodOptions(const moe::Environment& params) {
    Status ret = validateServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    if ((params.count("nodur") || params.count("nojournal")) &&
        (params.count("dur") || params.count("journal"))) {
        return Status(ErrorCodes::BadValue,
                      "Can't specify both --journal and --nojournal options.");
    }

    // SERVER-10019 Enabling rest/jsonp without --httpinterface should break in all cases in the
    // future
    if (params.count("net.http.RESTInterfaceEnabled") &&
        params["net.http.RESTInterfaceEnabled"].as<bool>() == true) {
        // If we are explicitly setting httpinterface to false in the config file (the source of
        // "net.http.enabled") and not overriding it on the command line (the source of
        // "httpinterface"), then we can fail with an error message without breaking backwards
        // compatibility.
        if (!params.count("httpinterface") && params.count("net.http.enabled") &&
            params["net.http.enabled"].as<bool>() == false) {
            return Status(ErrorCodes::BadValue,
                          "httpinterface must be enabled to use the rest api");
        }
    }

    if (params.count("net.http.JSONPEnabled") &&
        params["net.http.JSONPEnabled"].as<bool>() == true) {
        // If we are explicitly setting httpinterface to false in the config file (the source of
        // "net.http.enabled") and not overriding it on the command line (the source of
        // "httpinterface"), then we can fail with an error message without breaking backwards
        // compatibility.
        if (!params.count("httpinterface") && params.count("net.http.enabled") &&
            params["net.http.enabled"].as<bool>() == false) {
            return Status(ErrorCodes::BadValue, "httpinterface must be enabled to use jsonp");
        }
    }

#ifdef _WIN32
    if (params.count("install") || params.count("reinstall")) {
        if (params.count("storage.dbPath") &&
            !boost::filesystem::path(params["storage.dbPath"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "dbPath requires an absolute file path with Windows services");
        }
    }
#endif

    if (params.count("storage.queryableBackupMode")) {
        // Command line options that are disallowed when --queryableBackupMode is specified.
        for (const auto& disallowedOption : {"replSet",
                                             "configSvr",
                                             "upgrade",
                                             "repair",
                                             "profile",
                                             "master",
                                             "slave",
                                             "source",
                                             "only",
                                             "slavedelay",
                                             "journal",
                                             "storage.journal.enabled",
                                             "dur",
                                             "autoresync",
                                             "fastsync"}) {
            if (params.count(disallowedOption)) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Cannot specify both --queryableBackupMode and --"
                                            << disallowedOption);
            }
        }

        bool isClusterRoleShard = false;
        if (params.count("sharding.clusterRole")) {
            auto clusterRole = params["sharding.clusterRole"].as<std::string>();
            isClusterRoleShard = (clusterRole == "shardsvr");
        }

        if ((isClusterRoleShard || params.count("shardsvr")) &&
            !params.count("sharding._overrideShardIdentity")) {
            return Status(
                ErrorCodes::BadValue,
                "shardsvr cluster role with queryableBackupMode requires _overrideShardIdentity");
        }
    }

    return Status::OK();
}

Status canonicalizeMongodOptions(moe::Environment* params) {
    // Need to handle this before canonicalizing the general "server options", since
    // httpinterface and nohttpinterface are shared between mongos and mongod, but mongod has
    // extra validation required.
    if (params->count("net.http.RESTInterfaceEnabled") &&
        (*params)["net.http.RESTInterfaceEnabled"].as<bool>() == true) {
        bool httpEnabled = false;
        if (params->count("net.http.enabled")) {
            Status ret = params->get("net.http.enabled", &httpEnabled);
            if (!ret.isOK()) {
                return ret;
            }
        }
        if (params->count("nohttpinterface")) {
            log() << "** WARNING: Should not specify both --rest and --nohttpinterface"
                  << startupWarningsLog;
        } else if (!(params->count("httpinterface") ||
                     (params->count("net.http.enabled") && httpEnabled == true))) {
            log() << "** WARNING: --rest is specified without --httpinterface,"
                  << startupWarningsLog;
            log() << "**          enabling http interface" << startupWarningsLog;
            Status ret = params->set("httpinterface", moe::Value(true));
            if (!ret.isOK()) {
                return ret;
            }
        }
    }

    if (params->count("net.http.JSONPEnabled") &&
        (*params)["net.http.JSONPEnabled"].as<bool>() == true) {
        if (params->count("nohttpinterface")) {
            log() << "** WARNING: Should not specify both --jsonp and --nohttpinterface"
                  << startupWarningsLog;
        } else if (!params->count("httpinterface")) {
            log() << "** WARNING --jsonp is specified without --httpinterface,"
                  << startupWarningsLog;
            log() << "**         enabling http interface" << startupWarningsLog;
            Status ret = params->set("httpinterface", moe::Value(true));
            if (!ret.isOK()) {
                return ret;
            }
        }
    }

    Status ret = canonicalizeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

#ifdef MONGO_CONFIG_SSL
    ret = canonicalizeSSLServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }
#endif

    // "storage.journal.enabled" comes from the config file, so override it if any of "journal",
    // "nojournal", "dur", and "nodur" are set, since those come from the command line.
    if (params->count("journal")) {
        Status ret =
            params->set("storage.journal.enabled", moe::Value((*params)["journal"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("journal");
        if (!ret.isOK()) {
            return ret;
        }
    }
    if (params->count("nojournal")) {
        Status ret =
            params->set("storage.journal.enabled", moe::Value(!(*params)["nojournal"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("nojournal");
        if (!ret.isOK()) {
            return ret;
        }
    }
    if (params->count("dur")) {
        Status ret =
            params->set("storage.journal.enabled", moe::Value((*params)["dur"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("dur");
        if (!ret.isOK()) {
            return ret;
        }
    }
    if (params->count("nodur")) {
        Status ret =
            params->set("storage.journal.enabled", moe::Value(!(*params)["nodur"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("nodur");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // "storage.mmapv1.journal.durOptions" comes from the config file, so override it
    // if "durOptions" is set since that comes from the command line.
    if (params->count("durOptions")) {
        int durOptions;
        Status ret = params->get("durOptions", &durOptions);
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("durOptions");
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->set("storage.mmapv1.journal.debugFlags", moe::Value(durOptions));
        if (!ret.isOK()) {
            return ret;
        }
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

    // "storage.mmapv1.preallocDataFiles" comes from the config file, so override it if "noprealloc"
    // is set since that comes from the command line.
    if (params->count("noprealloc")) {
        Status ret = params->set("storage.mmapv1.preallocDataFiles",
                                 moe::Value(!(*params)["noprealloc"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("noprealloc");
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

    // "storage.indexBuildRetry" comes from the config file, so override it if
    // "noIndexBuildRetry" is set since that comes from the command line.
    if (params->count("noIndexBuildRetry")) {
        Status ret = params->set("storage.indexBuildRetry",
                                 moe::Value(!(*params)["noIndexBuildRetry"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("noIndexBuildRetry");
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

Status storeMongodOptions(const moe::Environment& params, const std::vector<std::string>& args) {
    Status ret = storeServerOptions(params, args);
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
        storageGlobalParams.engine = params["storage.engine"].as<string>();
        storageGlobalParams.engineSetByUser = true;
    }

    if (params.count("storage.dbPath")) {
        storageGlobalParams.dbpath = params["storage.dbPath"].as<string>();
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

    if (params.count("operationProfiling.slowOpThresholdMs")) {
        serverGlobalParams.slowMS = params["operationProfiling.slowOpThresholdMs"].as<int>();
    }

    if (params.count("storage.syncPeriodSecs")) {
        storageGlobalParams.syncdelay = params["storage.syncPeriodSecs"].as<double>();
    }

    if (params.count("storage.directoryPerDB")) {
        storageGlobalParams.directoryperdb = params["storage.directoryPerDB"].as<bool>();
    }

    if (params.count("storage.queryableBackupMode") &&
        params["storage.queryableBackupMode"].as<bool>()) {
        storageGlobalParams.readOnly = true;
        storageGlobalParams.dur = false;
    }

    if (params.count("cpu")) {
        serverGlobalParams.cpu = params["cpu"].as<bool>();
    }
    if (params.count("storage.mmapv1.quota.enforced")) {
        mmapv1GlobalOptions.quota = params["storage.mmapv1.quota.enforced"].as<bool>();
    }
    if (params.count("storage.mmapv1.quota.maxFilesPerDB")) {
        mmapv1GlobalOptions.quota = true;
        mmapv1GlobalOptions.quotaFiles = params["storage.mmapv1.quota.maxFilesPerDB"].as<int>() - 1;
    }

    if (params.count("storage.journal.enabled")) {
        storageGlobalParams.dur = params["storage.journal.enabled"].as<bool>();
    }

    if (params.count("storage.journal.commitIntervalMs")) {
        // don't check if dur is false here as many will just use the default, and will default
        // to off on win32.  ie no point making life a little more complex by giving an error on
        // a dev environment.
        storageGlobalParams.journalCommitIntervalMs =
            params["storage.journal.commitIntervalMs"].as<int>();
        if (storageGlobalParams.journalCommitIntervalMs < 1 ||
            storageGlobalParams.journalCommitIntervalMs >
                StorageGlobalParams::kMaxJournalCommitIntervalMs) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "--journalCommitInterval out of allowed range (1-"
                                        << StorageGlobalParams::kMaxJournalCommitIntervalMs
                                        << "ms)");
        }
    }
    if (params.count("storage.mmapv1.journal.debugFlags")) {
        mmapv1GlobalOptions.journalOptions = params["storage.mmapv1.journal.debugFlags"].as<int>();
    }
    if (params.count("nopreallocj")) {
        mmapv1GlobalOptions.preallocj = !params["nopreallocj"].as<bool>();
    }

    if (params.count("net.http.RESTInterfaceEnabled")) {
        serverGlobalParams.rest = params["net.http.RESTInterfaceEnabled"].as<bool>();
    }
    if (params.count("net.http.JSONPEnabled")) {
        serverGlobalParams.jsonp = params["net.http.JSONPEnabled"].as<bool>();
    }
    if (params.count("security.javascriptEnabled")) {
        mongodGlobalParams.scriptingEnabled = params["security.javascriptEnabled"].as<bool>();
    }
    if (params.count("storage.mmapv1.preallocDataFiles")) {
        mmapv1GlobalOptions.prealloc = params["storage.mmapv1.preallocDataFiles"].as<bool>();
        cout << "note: noprealloc may hurt performance in many applications" << endl;
    }
    if (params.count("storage.mmapv1.smallFiles")) {
        mmapv1GlobalOptions.smallfiles = params["storage.mmapv1.smallFiles"].as<bool>();
    }
    if (params.count("diaglog")) {
        warning() << "--diaglog is deprecated and will be removed in a future release"
                  << startupWarningsLog;
        int x = params["diaglog"].as<int>();
        if (x < 0 || x > 7) {
            return Status(ErrorCodes::BadValue, "can't interpret --diaglog setting");
        }
        _diaglog.setLevel(x);
    }

    if ((params.count("storage.journal.enabled") &&
         params["storage.journal.enabled"].as<bool>() == true) &&
        params.count("repair")) {
        return Status(ErrorCodes::BadValue,
                      "Can't have journaling enabled when using --repair option.");
    }

    if (params.count("repair") && params["repair"].as<bool>() == true) {
        storageGlobalParams.upgrade = 1;  // --repair implies --upgrade
        storageGlobalParams.repair = 1;
        storageGlobalParams.dur = false;
    }
    if (params.count("upgrade") && params["upgrade"].as<bool>() == true) {
        storageGlobalParams.upgrade = 1;
    }
    if (params.count("notablescan")) {
        storageGlobalParams.noTableScan = params["notablescan"].as<bool>();
    }

    repl::ReplSettings replSettings;
    if (params.count("master")) {
        replSettings.setMaster(params["master"].as<bool>());
    }
    if (params.count("slave") && params["slave"].as<bool>() == true) {
        replSettings.setSlave(true);
    }
    if (params.count("slavedelay")) {
        replSettings.setSlaveDelaySecs(params["slavedelay"].as<int>());
    }
    if (params.count("fastsync")) {
        if (!replSettings.isSlave()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "--fastsync must only be used with --slave");
        }
        replSettings.setFastSyncEnabled(params["fastsync"].as<bool>());
    }
    if (params.count("autoresync")) {
        replSettings.setAutoResyncEnabled(params["autoresync"].as<bool>());
    }
    if (params.count("source")) {
        /* specifies what the source in local.sources should be */
        replSettings.setSource(params["source"].as<string>().c_str());
    }
    if (params.count("pretouch")) {
        replSettings.setPretouch(params["pretouch"].as<int>());
    }
    if (params.count("replication.replSetName")) {
        replSettings.setReplSetString(params["replication.replSetName"].as<string>().c_str());
    }
    if (params.count("replication.replSet")) {
        /* seed list of hosts for the repl set */
        replSettings.setReplSetString(params["replication.replSet"].as<string>().c_str());
    }
    if (params.count("replication.secondaryIndexPrefetch")) {
        replSettings.setPrefetchIndexMode(
            params["replication.secondaryIndexPrefetch"].as<std::string>());
    }

    if (params.count("replication.enableMajorityReadConcern")) {
        replSettings.setMajorityReadConcernEnabled(
            params["replication.enableMajorityReadConcern"].as<bool>());
    }

    if (params.count("storage.indexBuildRetry")) {
        serverGlobalParams.indexBuildRetry = params["storage.indexBuildRetry"].as<bool>();
    }

    if (params.count("only")) {
        replSettings.setOnly(params["only"].as<string>().c_str());
    }
    if (params.count("storage.mmapv1.nsSize")) {
        int x = params["storage.mmapv1.nsSize"].as<int>();
        if (x <= 0 || x > (0x7fffffff / 1024 / 1024)) {
            return Status(ErrorCodes::BadValue, "bad --nssize arg");
        }
        mmapv1GlobalOptions.lenForNewNsFiles = x * 1024 * 1024;
        verify(mmapv1GlobalOptions.lenForNewNsFiles > 0);
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
        if (serverGlobalParams.port <= 0 || serverGlobalParams.port > 65535) {
            return Status(ErrorCodes::BadValue, "bad --port number");
        }
    }
    if (params.count("sharding.clusterRole")) {
        auto clusterRoleParam = params["sharding.clusterRole"].as<std::string>();
        if (clusterRoleParam == "configsvr") {
            bool journal = true;
            params.get("storage.journal.enabled", &journal);
            if (!journal) {
                return Status(ErrorCodes::BadValue,
                              "journaling cannot be turned off when configsvr is specified");
            }
            serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
            replSettings.setMajorityReadConcernEnabled(true);

            // If we haven't explicitly specified a journal option, default journaling to true for
            // the config server role
            if (!params.count("storage.journal.enabled")) {
                storageGlobalParams.dur = true;
            }

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

    // needs to be after things like --configsvr parsing, thus here.
    if (params.count("storage.repairPath")) {
        storageGlobalParams.repairpath = params["storage.repairPath"].as<string>();
        if (!storageGlobalParams.repairpath.size()) {
            return Status(ErrorCodes::BadValue, "repairpath is empty");
        }

        if (storageGlobalParams.dur &&
            !str::startsWith(storageGlobalParams.repairpath, storageGlobalParams.dbpath)) {
            return Status(ErrorCodes::BadValue,
                          "You must use a --repairpath that is a subdirectory of --dbpath when "
                          "using journaling");
        }
    } else {
        storageGlobalParams.repairpath = storageGlobalParams.dbpath;
    }

    if (replSettings.getPretouch())
        log() << "--pretouch " << replSettings.getPretouch();

    // Check if we are 32 bit and have not explicitly specified any journaling options
    if (sizeof(void*) == 4 && !params.count("storage.journal.enabled")) {
        // trying to make this stand out more like startup warnings
        log() << endl;
        warning() << "32-bit servers don't have journaling enabled by default. "
                  << "Please use --journal if you want durability." << endl;
        log() << endl;
    }

#ifdef _WIN32
    // If dbPath is a default value, prepend with drive name so log entries are explicit
    if (storageGlobalParams.dbpath == storageGlobalParams.kDefaultDbPath ||
        storageGlobalParams.dbpath == storageGlobalParams.kDefaultConfigDbPath) {
        boost::filesystem::path currentPath = boost::filesystem::current_path();
        storageGlobalParams.dbpath = currentPath.root_name().string() + storageGlobalParams.dbpath;
    }
#endif

    setGlobalReplSettings(replSettings);
    return Status::OK();
}

namespace {
repl::ReplSettings globalReplSettings;
}  // namespace

void setGlobalReplSettings(const repl::ReplSettings& settings) {
    globalReplSettings = settings;
}

const repl::ReplSettings& getGlobalReplSettings() {
    return globalReplSettings;
}

}  // namespace mongo
