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

#include "mongo/db/mongod_options.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/instance.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/server_options.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/version.h"

namespace mongo {

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

#ifdef MONGO_SSL
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

        general_options.addOptionChaining("auth", "auth", moe::Switch, "run with security");

        general_options.addOptionChaining("cpu", "cpu", moe::Switch,
                "periodically show cpu and iowait utilization");

#ifdef _WIN32
        general_options.addOptionChaining("dbpath", "dbpath", moe::String,
                "directory for datafiles - defaults to \\data\\db\\")
                                         .setDefault(moe::Value(std::string("\\data\\db\\")));

#else
        general_options.addOptionChaining("dbpath", "dbpath", moe::String,
                "directory for datafiles - defaults to /data/db/")
                                         .setDefault(moe::Value(std::string("/data/db")));

#endif
        general_options.addOptionChaining("diaglog", "diaglog", moe::Int,
                "0=off 1=W 2=R 3=both 7=W+some reads");

        general_options.addOptionChaining("directoryperdb", "directoryperdb", moe::Switch,
                "each database will be stored in a separate directory");

        general_options.addOptionChaining("ipv6", "ipv6", moe::Switch,
                "enable IPv6 support (disabled by default)");

        general_options.addOptionChaining("journal", "journal", moe::Switch, "enable journaling");

        general_options.addOptionChaining("journalCommitInterval", "journalCommitInterval",
                moe::Unsigned, "how often to group/batch commit (ms)");

        general_options.addOptionChaining("journalOptions", "journalOptions", moe::Int,
                "journal diagnostic options");

        general_options.addOptionChaining("jsonp", "jsonp", moe::Switch,
                "allow JSONP access via http (has security implications)");

        general_options.addOptionChaining("noauth", "noauth", moe::Switch, "run without security");

        general_options.addOptionChaining("noIndexBuildRetry", "noIndexBuildRetry", moe::Switch,
                "don't retry any index builds that were interrupted by shutdown");

        general_options.addOptionChaining("nojournal", "nojournal", moe::Switch,
                "disable journaling (journaling is on by default for 64 bit)");

        general_options.addOptionChaining("noprealloc", "noprealloc", moe::Switch,
                "disable data file preallocation - will often hurt performance");

        general_options.addOptionChaining("noscripting", "noscripting", moe::Switch,
                "disable scripting engine");

        general_options.addOptionChaining("notablescan", "notablescan", moe::Switch,
                "do not allow table scans");

        general_options.addOptionChaining("nssize", "nssize", moe::Int,
                ".ns file size (in MB) for new databases")
                                         .setDefault(moe::Value(16));

        general_options.addOptionChaining("profile", "profile", moe::Int, "0=off 1=slow, 2=all");

        general_options.addOptionChaining("quota", "quota", moe::Switch,
                "limits each database to a certain number of files (8 default)");

        general_options.addOptionChaining("quotaFiles", "quotaFiles", moe::Int,
                "number of files allowed per db, requires --quota");

        general_options.addOptionChaining("repair", "repair", moe::Switch, "run repair on all dbs");

        general_options.addOptionChaining("repairpath", "repairpath", moe::String,
                "root directory for repair files - defaults to dbpath");

        general_options.addOptionChaining("rest", "rest", moe::Switch, "turn on simple rest api");

#if defined(__linux__)
        general_options.addOptionChaining("shutdown", "shutdown", moe::Switch,
                "kill a running server (for init scripts)");

#endif
        general_options.addOptionChaining("slowms", "slowms", moe::Int,
                "value of slow for profile and console log")
                                         .setDefault(moe::Value(100));

        general_options.addOptionChaining("smallfiles", "smallfiles", moe::Switch,
                "use a smaller default file size");

        general_options.addOptionChaining("syncdelay", "syncdelay", moe::Double,
                "seconds between disk syncs (0=never, but not recommended)")
                                         .setDefault(moe::Value(60.0));

        general_options.addOptionChaining("sysinfo", "sysinfo", moe::Switch,
                "print some diagnostic system information");

        general_options.addOptionChaining("upgrade", "upgrade", moe::Switch,
                "upgrade db if needed");


        replication_options.addOptionChaining("oplogSize", "oplogSize", moe::Int,
                "size to use (in MB) for replication op log. default is 5% of disk space "
                "(i.e. large is good)");


        ms_options.addOptionChaining("master", "master", moe::Switch, "master mode");

        ms_options.addOptionChaining("slave", "slave", moe::Switch, "slave mode");

        ms_options.addOptionChaining("source", "source", moe::String,
                "when slave: specify master as <server:port>");

        ms_options.addOptionChaining("only", "only", moe::String,
                "when slave: specify a single database to replicate");

        ms_options.addOptionChaining("slavedelay", "slavedelay", moe::Int,
                "specify delay (in seconds) to be used when applying master ops to slave");

        ms_options.addOptionChaining("autoresync", "autoresync", moe::Switch,
                "automatically resync if slave data is stale");


        rs_options.addOptionChaining("replSet", "replSet", moe::String,
                "arg is <setname>[/<optionalseedhostlist>]");

        rs_options.addOptionChaining("replIndexPrefetch", "replIndexPrefetch", moe::String,
                "specify index prefetching behavior (if secondary) [none|_id_only|all]");


        sharding_options.addOptionChaining("configsvr", "configsvr", moe::Switch,
                "declare this is a config db of a cluster; default port 27019; "
                "default dir /data/configdb");

        sharding_options.addOptionChaining("shardsvr", "shardsvr", moe::Switch,
                "declare this is a shard db of a cluster; default port 27018");


        sharding_options.addOptionChaining("noMoveParanoia", "noMoveParanoia", moe::Switch,
                "turn off paranoid saving of data for the moveChunk command; default")
                                          .hidden();

        sharding_options.addOptionChaining("moveParanoia", "moveParanoia", moe::Switch,
                "turn on paranoid saving of data during the moveChunk command "
                "(used for internal system diagnostics)")
                                          .hidden();

        options->addSection(general_options);
#if defined(_WIN32)
        options->addSection(windows_scm_options);
#endif
        options->addSection(replication_options);
        options->addSection(ms_options);
        options->addSection(rs_options);
        options->addSection(sharding_options);
#ifdef MONGO_SSL
        options->addSection(ssl_options);
#endif

        options->addOptionChaining("fastsync", "fastsync", moe::Switch,
                "indicate that this instance is starting from a dbpath snapshot of the repl peer")
                                  .hidden();

        options->addOptionChaining("pretouch", "pretouch", moe::Int,
                "n pretouch threads for applying master/slave operations")
                                  .hidden();

        // This is a deprecated option that we are supporting for backwards compatibility
        // The first value for this option can be either 'dbpath' or 'run'.
        // If it is 'dbpath', mongod prints the dbpath and exits.  Any extra values are ignored.
        // If it is 'run', mongod runs normally.  Providing extra values is an error.
        options->addOptionChaining("command", "command", moe::StringVector, "command")
                                  .hidden()
                                  .positional(1, 3);

        options->addOptionChaining("cacheSize", "cacheSize", moe::Long,
                "cache size (in MB) for rec store")
                                  .hidden();

        options->addOptionChaining("nodur", "nodur", moe::Switch, "disable journaling")
                                  .hidden();

        // things we don't want people to use
        options->addOptionChaining("nohints", "nohints", moe::Switch, "ignore query hints")
                                  .hidden();

        options->addOptionChaining("nopreallocj", "nopreallocj", moe::Switch,
                "don't preallocate journal files")
                                  .hidden();

        options->addOptionChaining("dur", "dur", moe::Switch, "enable journaling")
                                  .hidden();

        options->addOptionChaining("durOptions", "durOptions", moe::Int,
                "durability diagnostic options")
                                  .hidden();

        // deprecated pairing command line options
        options->addOptionChaining("pairwith", "pairwith", moe::Switch, "DEPRECATED")
                                  .hidden();

        options->addOptionChaining("arbiter", "arbiter", moe::Switch, "DEPRECATED")
                                  .hidden();

        options->addOptionChaining("opIdMem", "opIdMem", moe::Switch, "DEPRECATED")
                                  .hidden();

        return Status::OK();
    }

    void printMongodHelp(const moe::OptionSection& options) {
        std::cout << options.helpString() << std::endl;
    };

    namespace {
        void sysRuntimeInfo() {
            out() << "sysinfo:" << endl;
#if defined(_SC_PAGE_SIZE)
            out() << "  page size: " << (int) sysconf(_SC_PAGE_SIZE) << endl;
#endif
#if defined(_SC_PHYS_PAGES)
            out() << "  _SC_PHYS_PAGES: " << sysconf(_SC_PHYS_PAGES) << endl;
#endif
#if defined(_SC_AVPHYS_PAGES)
            out() << "  _SC_AVPHYS_PAGES: " << sysconf(_SC_AVPHYS_PAGES) << endl;
#endif
        }
    } // namespace

    bool handlePreValidationMongodOptions(const moe::Environment& params,
                                            const std::vector<std::string>& args) {
        if (params.count("help")) {
            printMongodHelp(moe::startupOptions);
            return false;
        }
        if (params.count("version")) {
            cout << mongodVersion() << endl;
            printGitVersion();
            printOpenSSLVersion();
            return false;
        }
        if (params.count("sysinfo")) {
            sysRuntimeInfo();
            return false;
        }

        return true;
    }

    Status storeMongodOptions(const moe::Environment& params,
                              const std::vector<std::string>& args) {

        Status ret = storeServerOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        if (params.count("dbpath")) {
            storageGlobalParams.dbpath = params["dbpath"].as<string>();
            if (params.count("fork") && storageGlobalParams.dbpath[0] != '/') {
                // we need to change dbpath if we fork since we change
                // cwd to "/"
                // fork only exists on *nix
                // so '/' is safe
                storageGlobalParams.dbpath = serverGlobalParams.cwd + "/" +
                                                 storageGlobalParams.dbpath;
            }
        }
#ifdef _WIN32
        if (storageGlobalParams.dbpath.size() > 1 &&
            storageGlobalParams.dbpath[storageGlobalParams.dbpath.size()-1] == '/') {
            // size() check is for the unlikely possibility of --dbpath "/"
            storageGlobalParams.dbpath =
                storageGlobalParams.dbpath.erase(storageGlobalParams.dbpath.size()-1);
        }
#endif
        if ( params.count("slowms")) {
            serverGlobalParams.slowMS = params["slowms"].as<int>();
        }

        if ( params.count("syncdelay")) {
            storageGlobalParams.syncdelay = params["syncdelay"].as<double>();
        }

        if (params.count("directoryperdb")) {
            storageGlobalParams.directoryperdb = true;
        }
        if (params.count("cpu")) {
            serverGlobalParams.cpu = true;
        }
        if (params.count("noauth")) {
            getGlobalAuthorizationManager()->setAuthEnabled(false);
        }
        if (params.count("auth")) {
            getGlobalAuthorizationManager()->setAuthEnabled(true);
        }
        if (params.count("quota")) {
            storageGlobalParams.quota = true;
        }
        if (params.count("quotaFiles")) {
            storageGlobalParams.quota = true;
            storageGlobalParams.quotaFiles = params["quotaFiles"].as<int>() - 1;
        }
        if ((params.count("nodur") || params.count("nojournal")) &&
            (params.count("dur") || params.count("journal"))) {
            return Status(ErrorCodes::BadValue,
                          "Can't specify both --journal and --nojournal options.");
        }

        if (params.count("nodur") || params.count("nojournal")) {
            storageGlobalParams.dur = false;
        }

        if (params.count("dur") || params.count("journal")) {
            storageGlobalParams.dur = true;
        }

        if (params.count("durOptions")) {
            storageGlobalParams.durOptions = params["durOptions"].as<int>();
        }
        if( params.count("journalCommitInterval") ) {
            // don't check if dur is false here as many will just use the default, and will default
            // to off on win32.  ie no point making life a little more complex by giving an error on
            // a dev environment.
            storageGlobalParams.journalCommitInterval =
                params["journalCommitInterval"].as<unsigned>();
            if (storageGlobalParams.journalCommitInterval <= 1 ||
                storageGlobalParams.journalCommitInterval > 300) {
                return Status(ErrorCodes::BadValue,
                              "--journalCommitInterval out of allowed range (0-300ms)");
            }
        }
        if (params.count("journalOptions")) {
            storageGlobalParams.durOptions = params["journalOptions"].as<int>();
        }
        if (params.count("nohints")) {
            storageGlobalParams.useHints = false;
        }
        if (params.count("nopreallocj")) {
            storageGlobalParams.preallocj = false;
        }
        if (params.count("httpinterface")) {
            if (params.count("nohttpinterface")) {
                return Status(ErrorCodes::BadValue,
                              "can't have both --httpinterface and --nohttpinterface");
            }
            serverGlobalParams.isHttpInterfaceEnabled = true;
        }
        // SERVER-10019 Enabling rest/jsonp without --httpinterface should break in the future
        if (params.count("rest")) {
            if (params.count("nohttpinterface")) {
                log() << "** WARNING: Should not specify both --rest and --nohttpinterface" <<
                    startupWarningsLog;
            }
            else if (!params.count("httpinterface")) {
                log() << "** WARNING: --rest is specified without --httpinterface," <<
                    startupWarningsLog;
                log() << "**          enabling http interface" << startupWarningsLog;
                serverGlobalParams.isHttpInterfaceEnabled = true;
            }
            serverGlobalParams.rest = true;
        }
        if (params.count("jsonp")) {
            if (params.count("nohttpinterface")) {
                log() << "** WARNING: Should not specify both --jsonp and --nohttpinterface" <<
                    startupWarningsLog;
            }
            else if (!params.count("httpinterface")) {
                log() << "** WARNING --jsonp is specified without --httpinterface," <<
                    startupWarningsLog;
                log() << "**         enabling http interface" << startupWarningsLog;
                serverGlobalParams.isHttpInterfaceEnabled = true;
            }
            serverGlobalParams.jsonp = true;
        }
        if (params.count("noscripting")) {
            mongodGlobalParams.scriptingEnabled = false;
        }
        if (params.count("noprealloc")) {
            storageGlobalParams.prealloc = false;
            cout << "note: noprealloc may hurt performance in many applications" << endl;
        }
        if (params.count("smallfiles")) {
            storageGlobalParams.smallfiles = true;
        }
        if (params.count("diaglog")) {
            int x = params["diaglog"].as<int>();
            if ( x < 0 || x > 7 ) {
                return Status(ErrorCodes::BadValue, "can't interpret --diaglog setting");
            }
            _diaglog.setLevel(x);
        }

        if ((params.count("dur") || params.count("journal")) && params.count("repair")) {
            return Status(ErrorCodes::BadValue,
                          "Can't specify both --journal and --repair options.");
        }

        if (params.count("repair")) {
            Record::MemoryTrackingEnabled = false;
            mongodGlobalParams.upgrade = 1; // --repair implies --upgrade
            mongodGlobalParams.repair = 1;
            storageGlobalParams.dur = false;
        }
        if (params.count("upgrade")) {
            Record::MemoryTrackingEnabled = false;
            mongodGlobalParams.upgrade = 1;
        }
        if (params.count("notablescan")) {
            storageGlobalParams.noTableScan = true;
        }
        if (params.count("master")) {
            replSettings.master = true;
        }
        if (params.count("slave")) {
            replSettings.slave = SimpleSlave;
        }
        if (params.count("slavedelay")) {
            replSettings.slavedelay = params["slavedelay"].as<int>();
        }
        if (params.count("fastsync")) {
            replSettings.fastsync = true;
        }
        if (params.count("autoresync")) {
            replSettings.autoresync = true;
            if( params.count("replSet") ) {
                return Status(ErrorCodes::BadValue,
                              "--autoresync is not used with --replSet\nsee "
                              "http://dochub.mongodb.org/core/resyncingaverystalereplicasetmember");
            }
        }
        if (params.count("source")) {
            /* specifies what the source in local.sources should be */
            replSettings.source = params["source"].as<string>().c_str();
        }
        if( params.count("pretouch") ) {
            replSettings.pretouch = params["pretouch"].as<int>();
        }
        if (params.count("replSet")) {
            if (params.count("slavedelay")) {
                return Status(ErrorCodes::BadValue, "--slavedelay cannot be used with --replSet");
            }
            else if (params.count("only")) {
                return Status(ErrorCodes::BadValue, "--only cannot be used with --replSet");
            }
            /* seed list of hosts for the repl set */
            replSettings.replSet = params["replSet"].as<string>().c_str();
        }
        if (params.count("replIndexPrefetch")) {
            replSettings.rsIndexPrefetch = params["replIndexPrefetch"].as<std::string>();
        }
        if (params.count("noIndexBuildRetry")) {
            serverGlobalParams.indexBuildRetry = false;
        }
        if (params.count("only")) {
            replSettings.only = params["only"].as<string>().c_str();
        }
        if( params.count("nssize") ) {
            int x = params["nssize"].as<int>();
            if (x <= 0 || x > (0x7fffffff/1024/1024)) {
                return Status(ErrorCodes::BadValue, "bad --nssize arg");
            }
            storageGlobalParams.lenForNewNsFiles = x * 1024 * 1024;
            verify(storageGlobalParams.lenForNewNsFiles > 0);
        }
        if (params.count("oplogSize")) {
            long long x = params["oplogSize"].as<int>();
            if (x <= 0) {
                return Status(ErrorCodes::BadValue, "bad --oplogSize arg");
            }
            // note a small size such as x==1 is ok for an arbiter.
            if( x > 1000 && sizeof(void*) == 4 ) {
                StringBuilder sb;
                sb << "--oplogSize of " << x
                   << "MB is too big for 32 bit version. Use 64 bit build instead.";
                return Status(ErrorCodes::BadValue, sb.str());
            }
            replSettings.oplogSize = x * 1024 * 1024;
            verify(replSettings.oplogSize > 0);
        }
        if (params.count("cacheSize")) {
            long x = params["cacheSize"].as<long>();
            if (x <= 0) {
                return Status(ErrorCodes::BadValue, "bad --cacheSize arg");
            }
            return Status(ErrorCodes::BadValue, "--cacheSize option not currently supported");
        }
        if (!params.count("port")) {
            if( params.count("configsvr") ) {
                serverGlobalParams.port = ServerGlobalParams::ConfigServerPort;
            }
            if( params.count("shardsvr") ) {
                if( params.count("configsvr") ) {
                    return Status(ErrorCodes::BadValue,
                                  "can't do --shardsvr and --configsvr at the same time");
                }
                serverGlobalParams.port = ServerGlobalParams::ShardServerPort;
            }
        }
        else {
            if (serverGlobalParams.port <= 0 || serverGlobalParams.port > 65535) {
                return Status(ErrorCodes::BadValue, "bad --port number");
            }
        }
        if ( params.count("configsvr" ) ) {
            serverGlobalParams.configsvr = true;
            storageGlobalParams.smallfiles = true; // config server implies small files
            if (replSettings.usingReplSets() || replSettings.master || replSettings.slave) {
                return Status(ErrorCodes::BadValue,
                              "replication should not be enabled on a config server");
            }
            if (!params.count("nodur") && !params.count("nojournal"))
                storageGlobalParams.dur = true;
            if (!params.count("dbpath"))
                storageGlobalParams.dbpath = "/data/configdb";
            replSettings.master = true;
            if (!params.count("oplogSize"))
                replSettings.oplogSize = 5 * 1024 * 1024;
        }
        if ( params.count( "profile" ) ) {
            serverGlobalParams.defaultProfile = params["profile"].as<int>();
        }
        if (params.count("ipv6")) {
            enableIPv6();
        }

        if (params.count("noMoveParanoia") && params.count("moveParanoia")) {
            return Status(ErrorCodes::BadValue,
                          "The moveParanoia and noMoveParanoia flags cannot both be set");
        }

        if (params.count("noMoveParanoia"))
            serverGlobalParams.moveParanoia = false;

        if (params.count("moveParanoia"))
            serverGlobalParams.moveParanoia = true;

        if (params.count("pairwith") || params.count("arbiter") || params.count("opIdMem")) {
            return Status(ErrorCodes::BadValue,
                          "****\n"
                          "Replica Pairs have been deprecated. Invalid options: "
                              "--pairwith, --arbiter, and/or --opIdMem\n"
                          "<http://dochub.mongodb.org/core/replicapairs>\n"
                          "****");
        }

        // needs to be after things like --configsvr parsing, thus here.
        if (params.count("repairpath")) {
            storageGlobalParams.repairpath = params["repairpath"].as<string>();
            if (!storageGlobalParams.repairpath.size()) {
                return Status(ErrorCodes::BadValue, "repairpath is empty");
            }

            if (storageGlobalParams.dur &&
                !str::startsWith(storageGlobalParams.repairpath,
                                 storageGlobalParams.dbpath)) {
                return Status(ErrorCodes::BadValue,
                              "You must use a --repairpath that is a subdirectory of --dbpath when "
                              "using journaling");
            }
        }
        else {
            storageGlobalParams.repairpath = storageGlobalParams.dbpath;
        }

        if (replSettings.pretouch)
            log() << "--pretouch " << replSettings.pretouch << endl;

        if (sizeof(void*) == 4 && !(params.count("nodur") || params.count("nojournal") ||
                                    params.count("dur") || params.count("journal"))) {
            // trying to make this stand out more like startup warnings
            log() << endl;
            warning() << "32-bit servers don't have journaling enabled by default. "
                      << "Please use --journal if you want durability." << endl;
            log() << endl;
        }

        return Status::OK();
    }

} // namespace mongo
