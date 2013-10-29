/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/db/mongod_options.h"

#include "mongo/bson/util/builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_options.h"

namespace {

    namespace moe = ::mongo::optionenvironment;

    enum DottedName {
        nopreallocj,
        fastsync,
        pretouch,
        command,
        cacheSize,
        nohints,
        pairwith,
        arbiter,
        opIdMem,
        help,
        version,
        config,
        verbose,
        systemLog_verbosity,
        systemLog_quiet,
        net_port,
        net_bindIp,
        net_maxIncomingConnections,
        logpath,
        systemLog_path,
        systemLog_destination,
        systemLog_syslogFacility,
        systemLog_logAppend,
        systemLog_timeStampFormat,
        processManagement_pidFilePath,
        security_keyFile,
        setParameter,
        httpinterface,
        net_http_enabled,
        net_http_port,
        net_http_RESTInterfaceEnabled,
        net_http_JSONPEnabled,
        security_clusterAuthMode,
        nounixsocket,
        net_unixDomainSocket_enabled,
        net_unixDomainSocket_pathPrefix,
        processManagement_fork,
        systemLog_syslog,
        vv,
        vvv,
        vvvv,
        vvvvv,
        vvvvvv,
        vvvvvvv,
        vvvvvvvv,
        vvvvvvvvv,
        vvvvvvvvvv,
        vvvvvvvvvvv,
        vvvvvvvvvvvv,
        nohttpinterface,
        objcheck,
        noobjcheck,
        net_wireObjectCheck,
        systemLog_traceAllExceptions,
        enableExperimentalIndexStatsCmd,
        enableExperimentalStorageDetailsCmd,
        auth,
        noauth,
        security_authentication,
        security_authSchemaVersion,
        security_authenticationMechanisms,
        security_enableLocalhostAuthBypass,
        security_supportCompatibilityFormPrivilegeDocuments,
        net_ipv6,
        diaglog,
        operationProfiling_slowOpThresholdMs,
        profile,
        operationProfiling_mode,
        cpu,
        sysinfo,
        storage_dbPath,
        storage_directoryPerDB,
        noIndexBuildRetry,
        storage_indexBuildRetry,
        noprealloc,
        storage_preallocDataFiles,
        storage_nsSize,
        storage_quota_enforced,
        storage_quota_maxFilesPerDB,
        storage_smallFiles,
        storage_syncPeriodSecs,
        upgrade,
        repair,
        storage_repairPath,
        noscripting,
        notablescan,
        journal,
        nojournal,
        dur,
        nodur,
        storage_journal_enabled,
        storage_journal_debugFlags,
        durOptions,
        storage_journal_commitIntervalMs,
        replication_oplogSizeMB,
        masterSlave_master,
        masterSlave_slave,
        masterSlave_source,
        masterSlave_only,
        masterSlave_slavedelay,
        masterSlave_autoresync,
        replication_replSet,
        replication_replSetName,
        replication_secondaryIndexPrefetch,
        sharding_configsvr,
        sharding_shardsvr,
        sharding_clusterRole,
        sharding_noMoveParanoia,
        sharding_archiveMovedChunks,
        auditLog_log,
        auditLog_format,
        auditLog_destination,
        auditLog_path,
        auditLog_filter,
        snmp_subagent,
        snmp_master,
        net_ssl_sslOnNormalPorts,
        net_ssl_mode,
        net_ssl_PEMKeyFile,
        net_ssl_PEMKeyPassword,
        net_ssl_clusterFile,
        net_ssl_clusterPassword,
        net_ssl_CAFile,
        net_ssl_CRLFile,
        net_ssl_weakCertificateValidation,
        net_ssl_allowInvalidCertificates,
        net_ssl_FIPSMode,
        install,
        remove,
        reinstall,
        processManagement_windowsService_serviceName,
        processManagement_windowsService_displayName,
        processManagement_windowsService_description,
        processManagement_windowsService_serviceUser,
        processManagement_windowsService_servicePassword,
        service,
        shutdown
    };

    TEST(Registration, RegisterAllOptions) {

        moe::OptionSection options;

        ASSERT_OK(::mongo::addMongodOptions(&options));

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(options.getAllOptions(&options_vector));

        std::map<std::string, DottedName> dottedNameMap;

        dottedNameMap["nopreallocj"] = nopreallocj;
        dottedNameMap["fastsync"] = fastsync;
        dottedNameMap["pretouch"] = pretouch;
        dottedNameMap["command"] = command;
        dottedNameMap["cacheSize"] = cacheSize;
        dottedNameMap["nohints"] = nohints;
        dottedNameMap["pairwith"] = pairwith;
        dottedNameMap["arbiter"] = arbiter;
        dottedNameMap["opIdMem"] = opIdMem;
        dottedNameMap["help"] = help;
        dottedNameMap["version"] = version;
        dottedNameMap["config"] = config;
        dottedNameMap["verbose"] = verbose;
        dottedNameMap["systemLog.verbosity"] = systemLog_verbosity;
        dottedNameMap["systemLog.quiet"] = systemLog_quiet;
        dottedNameMap["net.port"] = net_port;
        dottedNameMap["net.bindIp"] = net_bindIp;
        dottedNameMap["net.maxIncomingConnections"] = net_maxIncomingConnections;
        dottedNameMap["logpath"] = logpath;
        dottedNameMap["systemLog.path"] = systemLog_path;
        dottedNameMap["systemLog.destination"] = systemLog_destination;
        dottedNameMap["systemLog.syslogFacility"] = systemLog_syslogFacility;
        dottedNameMap["systemLog.logAppend"] = systemLog_logAppend;
        dottedNameMap["systemLog.timeStampFormat"] = systemLog_timeStampFormat;
        dottedNameMap["processManagement.pidFilePath"] = processManagement_pidFilePath;
        dottedNameMap["security.keyFile"] = security_keyFile;
        dottedNameMap["setParameter"] = setParameter;
        dottedNameMap["httpinterface"] = httpinterface;
        dottedNameMap["net.http.enabled"] = net_http_enabled;
        dottedNameMap["net.http.port"] = net_http_port;
        dottedNameMap["net.http.RESTInterfaceEnabled"] = net_http_RESTInterfaceEnabled;
        dottedNameMap["net.http.JSONPEnabled"] = net_http_JSONPEnabled;
        dottedNameMap["security.clusterAuthMode"] = security_clusterAuthMode;
        dottedNameMap["nounixsocket"] = nounixsocket;
        dottedNameMap["net.unixDomainSocket.enabled"] = net_unixDomainSocket_enabled;
        dottedNameMap["net.unixDomainSocket.pathPrefix"] = net_unixDomainSocket_pathPrefix;
        dottedNameMap["processManagement.fork"] = processManagement_fork;
        dottedNameMap["systemLog.syslog"] = systemLog_syslog;
        dottedNameMap["vv"] = vv;
        dottedNameMap["vvv"] = vvv;
        dottedNameMap["vvvv"] = vvvv;
        dottedNameMap["vvvvv"] = vvvvv;
        dottedNameMap["vvvvvv"] = vvvvvv;
        dottedNameMap["vvvvvvv"] = vvvvvvv;
        dottedNameMap["vvvvvvvv"] = vvvvvvvv;
        dottedNameMap["vvvvvvvvv"] = vvvvvvvvv;
        dottedNameMap["vvvvvvvvvv"] = vvvvvvvvvv;
        dottedNameMap["vvvvvvvvvvv"] = vvvvvvvvvvv;
        dottedNameMap["vvvvvvvvvvvv"] = vvvvvvvvvvvv;
        dottedNameMap["nohttpinterface"] = nohttpinterface;
        dottedNameMap["objcheck"] = objcheck;
        dottedNameMap["noobjcheck"] = noobjcheck;
        dottedNameMap["net.wireObjectCheck"] = net_wireObjectCheck;
        dottedNameMap["systemLog.traceAllExceptions"] = systemLog_traceAllExceptions;
        dottedNameMap["enableExperimentalIndexStatsCmd"] = enableExperimentalIndexStatsCmd;
        dottedNameMap["enableExperimentalStorageDetailsCmd"] = enableExperimentalStorageDetailsCmd;
        dottedNameMap["auth"] = auth;
        dottedNameMap["noauth"] = noauth;
        dottedNameMap["security.authentication"] = security_authentication;
        dottedNameMap["security.authSchemaVersion"] = security_authSchemaVersion;
        dottedNameMap["security.authenticationMechanisms"] = security_authenticationMechanisms;
        dottedNameMap["security.enableLocalhostAuthBypass"] = security_enableLocalhostAuthBypass;
        dottedNameMap["security.supportCompatibilityFormPrivilegeDocuments"] = security_supportCompatibilityFormPrivilegeDocuments;
        dottedNameMap["net.ipv6"] = net_ipv6;
        dottedNameMap["diaglog"] = diaglog;
        dottedNameMap["operationProfiling.slowOpThresholdMs"] = operationProfiling_slowOpThresholdMs;
        dottedNameMap["profile"] = profile;
        dottedNameMap["operationProfiling.mode"] = operationProfiling_mode;
        dottedNameMap["cpu"] = cpu;
        dottedNameMap["sysinfo"] = sysinfo;
        dottedNameMap["storage.dbPath"] = storage_dbPath;
        dottedNameMap["storage.directoryPerDB"] = storage_directoryPerDB;
        dottedNameMap["noIndexBuildRetry"] = noIndexBuildRetry;
        dottedNameMap["storage.indexBuildRetry"] = storage_indexBuildRetry;
        dottedNameMap["noprealloc"] = noprealloc;
        dottedNameMap["storage.preallocDataFiles"] = storage_preallocDataFiles;
        dottedNameMap["storage.nsSize"] = storage_nsSize;
        dottedNameMap["storage.quota.enforced"] = storage_quota_enforced;
        dottedNameMap["storage.quota.maxFilesPerDB"] = storage_quota_maxFilesPerDB;
        dottedNameMap["storage.smallFiles"] = storage_smallFiles;
        dottedNameMap["storage.syncPeriodSecs"] = storage_syncPeriodSecs;
        dottedNameMap["upgrade"] = upgrade;
        dottedNameMap["repair"] = repair;
        dottedNameMap["storage.repairPath"] = storage_repairPath;
        dottedNameMap["noscripting"] = noscripting;
        dottedNameMap["notablescan"] = notablescan;
        dottedNameMap["journal"] = journal;
        dottedNameMap["nojournal"] = nojournal;
        dottedNameMap["dur"] = dur;
        dottedNameMap["nodur"] = nodur;
        dottedNameMap["storage.journal.enabled"] = storage_journal_enabled;
        dottedNameMap["storage.journal.debugFlags"] = storage_journal_debugFlags;
        dottedNameMap["durOptions"] = durOptions;
        dottedNameMap["storage.journal.commitIntervalMs"] = storage_journal_commitIntervalMs;
        dottedNameMap["replication.oplogSizeMB"] = replication_oplogSizeMB;
        dottedNameMap["master"] = masterSlave_master;
        dottedNameMap["slave"] = masterSlave_slave;
        dottedNameMap["source"] = masterSlave_source;
        dottedNameMap["only"] = masterSlave_only;
        dottedNameMap["slavedelay"] = masterSlave_slavedelay;
        dottedNameMap["autoresync"] = masterSlave_autoresync;
        dottedNameMap["replication.replSet"] = replication_replSet;
        dottedNameMap["replication.replSetName"] = replication_replSetName;
        dottedNameMap["replication.secondaryIndexPrefetch"] = replication_secondaryIndexPrefetch;
        dottedNameMap["sharding.configsvr"] = sharding_configsvr;
        dottedNameMap["sharding.shardsvr"] = sharding_shardsvr;
        dottedNameMap["sharding.clusterRole"] = sharding_clusterRole;
        dottedNameMap["sharding.noMoveParanoia"] = sharding_noMoveParanoia;
        dottedNameMap["sharding.archiveMovedChunks"] = sharding_archiveMovedChunks;
        dottedNameMap["auditLog.log"] = auditLog_log;
        dottedNameMap["auditLog.format"] = auditLog_format;
        dottedNameMap["auditLog.destination"] = auditLog_destination;
        dottedNameMap["auditLog.path"] = auditLog_path;
        dottedNameMap["auditLog.filter"] = auditLog_filter;
        dottedNameMap["snmp.subagent"] = snmp_subagent;
        dottedNameMap["snmp.master"] = snmp_master;
        dottedNameMap["net.ssl.sslOnNormalPorts"] = net_ssl_sslOnNormalPorts;
        dottedNameMap["net.ssl.mode"] = net_ssl_mode;
        dottedNameMap["net.ssl.PEMKeyFile"] = net_ssl_PEMKeyFile;
        dottedNameMap["net.ssl.PEMKeyPassword"] = net_ssl_PEMKeyPassword;
        dottedNameMap["net.ssl.clusterFile"] = net_ssl_clusterFile;
        dottedNameMap["net.ssl.clusterPassword"] = net_ssl_clusterPassword;
        dottedNameMap["net.ssl.CAFile"] = net_ssl_CAFile;
        dottedNameMap["net.ssl.CRLFile"] = net_ssl_CRLFile;
        dottedNameMap["net.ssl.weakCertificateValidation"] = net_ssl_weakCertificateValidation;
        dottedNameMap["net.ssl.allowInvalidCertificates"] = net_ssl_allowInvalidCertificates;
        dottedNameMap["net.ssl.FIPSMode"] = net_ssl_FIPSMode;
        dottedNameMap["install"] = install;
        dottedNameMap["remove"] = remove;
        dottedNameMap["reinstall"] = reinstall;
        dottedNameMap["processManagement.windowsService.serviceName"] = processManagement_windowsService_serviceName;
        dottedNameMap["processManagement.windowsService.displayName"] = processManagement_windowsService_displayName;
        dottedNameMap["processManagement.windowsService.description"] = processManagement_windowsService_description;
        dottedNameMap["processManagement.windowsService.serviceUser"] = processManagement_windowsService_serviceUser;
        dottedNameMap["processManagement.windowsService.servicePassword"] = processManagement_windowsService_servicePassword;
        dottedNameMap["service"] = service;
        dottedNameMap["shutdown"] = shutdown;

        for (std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
             iterator != options_vector.end(); iterator++) {

            switch (dottedNameMap[iterator->_dottedName])
            {
            case nopreallocj:
                ASSERT_EQUALS(iterator->_singleName, "nopreallocj");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "don't preallocate journal files");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case fastsync:
                ASSERT_EQUALS(iterator->_singleName, "fastsync");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "indicate that this instance is starting from a dbpath snapshot of the repl peer");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case pretouch:
                ASSERT_EQUALS(iterator->_singleName, "pretouch");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "n pretouch threads for applying master/slave operations");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case command:
                ASSERT_EQUALS(iterator->_singleName, "command");
                ASSERT_EQUALS(iterator->_type, moe::StringVector);
                ASSERT_EQUALS(iterator->_description, "command");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, 1);
                ASSERT_EQUALS(iterator->_positionalEnd, 3);
                break;
            case cacheSize:
                ASSERT_EQUALS(iterator->_singleName, "cacheSize");
                ASSERT_EQUALS(iterator->_type, moe::Long);
                ASSERT_EQUALS(iterator->_description, "cache size (in MB) for rec store");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case nohints:
                ASSERT_EQUALS(iterator->_singleName, "nohints");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "ignore query hints");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case pairwith:
                ASSERT_EQUALS(iterator->_singleName, "pairwith");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "DEPRECATED");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case arbiter:
                ASSERT_EQUALS(iterator->_singleName, "arbiter");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "DEPRECATED");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case opIdMem:
                ASSERT_EQUALS(iterator->_singleName, "opIdMem");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "DEPRECATED");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case help:
                ASSERT_EQUALS(iterator->_singleName, "help,h");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "show this usage information");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case version:
                ASSERT_EQUALS(iterator->_singleName, "version");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "show version information");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case config:
                ASSERT_EQUALS(iterator->_singleName, "config,f");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "configuration file specifying additional options");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case verbose:
                ASSERT_EQUALS(iterator->_singleName, "verbose,v");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "be more verbose (include multiple times for more verbosity e.g. -vvvvv)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.equal(moe::Value(std::string("v"))));
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_verbosity:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "set verbose level");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_quiet:
                ASSERT_EQUALS(iterator->_singleName, "quiet");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "quieter output");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_port:
                ASSERT_EQUALS(iterator->_singleName, "port");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "specify port number - 27017 by default");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_bindIp:
                ASSERT_EQUALS(iterator->_singleName, "bind_ip");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "comma separated list of ip addresses to listen on - all local ips by default");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_maxIncomingConnections:
                ASSERT_EQUALS(iterator->_singleName, "maxConns");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "max number of simultaneous connections - 20000 by default");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case logpath:
                ASSERT_EQUALS(iterator->_singleName, "logpath");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "log file to send write to instead of stdout - has to be a file, not directory");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_path:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "log file to send writes to if logging to a file - has to be a file, not directory");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_destination:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Destination of system log output.  (syslog/file)");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_syslogFacility:
                ASSERT_EQUALS(iterator->_singleName, "syslogFacility");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "syslog facility used for monogdb syslog message");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_logAppend:
                ASSERT_EQUALS(iterator->_singleName, "logappend");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "append to logpath instead of over-writing");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_timeStampFormat:
                ASSERT_EQUALS(iterator->_singleName, "timeStampFormat");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Desired format for timestamps in log messages. One of ctime, iso8601-utc or iso8601-local");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case processManagement_pidFilePath:
                ASSERT_EQUALS(iterator->_singleName, "pidfilepath");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "full path to pidfile (if not set, no pidfile is created)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case security_keyFile:
                ASSERT_EQUALS(iterator->_singleName, "keyFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "private key for cluster authentication");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case setParameter:
                ASSERT_EQUALS(iterator->_singleName, "setParameter");
                ASSERT_EQUALS(iterator->_type, moe::StringMap);
                ASSERT_EQUALS(iterator->_description, "Set a configurable parameter");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, true);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case httpinterface:
                ASSERT_EQUALS(iterator->_singleName, "httpinterface");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "enable http interface");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_http_enabled:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Bool);
                ASSERT_EQUALS(iterator->_description, "enable http interface");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_http_port:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "port to listen on for http interface");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_http_RESTInterfaceEnabled:
                ASSERT_EQUALS(iterator->_singleName, "rest");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "turn on simple rest api");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_http_JSONPEnabled:
                ASSERT_EQUALS(iterator->_singleName, "jsonp");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "allow JSONP access via http (has security implications)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case security_clusterAuthMode:
                ASSERT_EQUALS(iterator->_singleName, "clusterAuthMode");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Authentication mode used for cluster authentication. Alternatives are (keyFile|sendKeyFile|sendX509|x509)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case nounixsocket:
                ASSERT_EQUALS(iterator->_singleName, "nounixsocket");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable listening on unix sockets");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_unixDomainSocket_enabled:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Bool);
                ASSERT_EQUALS(iterator->_description, "disable listening on unix sockets");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_unixDomainSocket_pathPrefix:
                ASSERT_EQUALS(iterator->_singleName, "unixSocketPrefix");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "alternative directory for UNIX domain sockets (defaults to /tmp)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case processManagement_fork:
                ASSERT_EQUALS(iterator->_singleName, "fork");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "fork server process");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_syslog:
                ASSERT_EQUALS(iterator->_singleName, "syslog");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "log to system's syslog facility instead of file or stdout");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vv:
                ASSERT_EQUALS(iterator->_singleName, "vv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvv:
                ASSERT_EQUALS(iterator->_singleName, "vvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvvvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvvvvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvvvvvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvvvvvvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvvvvvvvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case vvvvvvvvvvvv:
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case nohttpinterface:
                ASSERT_EQUALS(iterator->_singleName, "nohttpinterface");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable http interface");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case objcheck:
                ASSERT_EQUALS(iterator->_singleName, "objcheck");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "inspect client data for validity on receipt (DEFAULT)");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case noobjcheck:
                ASSERT_EQUALS(iterator->_singleName, "noobjcheck");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "do NOT inspect client data for validity on receipt");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_wireObjectCheck:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Bool);
                ASSERT_EQUALS(iterator->_description, "inspect client data for validity on receipt (DEFAULT)");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case systemLog_traceAllExceptions:
                ASSERT_EQUALS(iterator->_singleName, "traceExceptions");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "log stack traces for every exception");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case enableExperimentalIndexStatsCmd:
                ASSERT_EQUALS(iterator->_singleName, "enableExperimentalIndexStatsCmd");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "EXPERIMENTAL (UNSUPPORTED). Enable command computing aggregate statistics on indexes.");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case enableExperimentalStorageDetailsCmd:
                ASSERT_EQUALS(iterator->_singleName, "enableExperimentalStorageDetailsCmd");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "EXPERIMENTAL (UNSUPPORTED). Enable command computing aggregate statistics on storage.");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case auth:
                ASSERT_EQUALS(iterator->_singleName, "auth");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "run with security");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case noauth:
                ASSERT_EQUALS(iterator->_singleName, "noauth");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "run without security");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case security_authentication:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "How the database behaves with respect to authentication of clients.  Options are \"optional\", which means that a client can connect with or without authentication, and \"required\" which means clients must use authentication");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case security_authSchemaVersion:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "TODO");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case security_authenticationMechanisms:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "TODO");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case security_enableLocalhostAuthBypass:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "TODO");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case security_supportCompatibilityFormPrivilegeDocuments:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "TODO");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ipv6:
                ASSERT_EQUALS(iterator->_singleName, "ipv6");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "enable IPv6 support (disabled by default)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case diaglog:
                ASSERT_EQUALS(iterator->_singleName, "diaglog");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "DEPRECATED: 0=off 1=W 2=R 3=both 7=W+some reads");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case operationProfiling_slowOpThresholdMs:
                ASSERT_EQUALS(iterator->_singleName, "slowms");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "value of slow for profile and console log");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.equal(moe::Value(100)));
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case profile:
                ASSERT_EQUALS(iterator->_singleName, "profile");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "0=off 1=slow, 2=all");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case operationProfiling_mode:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "(off/slowOp/all)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case cpu:
                ASSERT_EQUALS(iterator->_singleName, "cpu");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "periodically show cpu and iowait utilization");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case sysinfo:
                ASSERT_EQUALS(iterator->_singleName, "sysinfo");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "print some diagnostic system information");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_dbPath:
                ASSERT_EQUALS(iterator->_singleName, "dbpath");
                ASSERT_EQUALS(iterator->_type, moe::String);
#ifdef _WIN32
                ASSERT_EQUALS(iterator->_description, "directory for datafiles - defaults to \\data\\db\\");
#else
                ASSERT_EQUALS(iterator->_description, "directory for datafiles - defaults to /data/db/");
#endif
                ASSERT_EQUALS(iterator->_isVisible, true);
#ifdef _WIN32
                ASSERT_TRUE(iterator->_default.equal(moe::Value(std::string("\\data\\db\\"))));
#else
                ASSERT_TRUE(iterator->_default.equal(moe::Value(std::string("/data/db"))));
#endif
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_directoryPerDB:
                ASSERT_EQUALS(iterator->_singleName, "directoryperdb");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "each database will be stored in a separate directory");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case noIndexBuildRetry:
                ASSERT_EQUALS(iterator->_singleName, "noIndexBuildRetry");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "don't retry any index builds that were interrupted by shutdown");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_indexBuildRetry:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Bool);
                ASSERT_EQUALS(iterator->_description, "don't retry any index builds that were interrupted by shutdown");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case noprealloc:
                ASSERT_EQUALS(iterator->_singleName, "noprealloc");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable data file preallocation - will often hurt performance");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_preallocDataFiles:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Bool);
                ASSERT_EQUALS(iterator->_description, "disable data file preallocation - will often hurt performance");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_nsSize:
                ASSERT_EQUALS(iterator->_singleName, "nssize");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, ".ns file size (in MB) for new databases");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.equal(moe::Value(16)));
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_quota_enforced:
                ASSERT_EQUALS(iterator->_singleName, "quota");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "limits each database to a certain number of files (8 default)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_quota_maxFilesPerDB:
                ASSERT_EQUALS(iterator->_singleName, "quotaFiles");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "number of files allowed per db, implies --quota");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_smallFiles:
                ASSERT_EQUALS(iterator->_singleName, "smallfiles");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "use a smaller default file size");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_syncPeriodSecs:
                ASSERT_EQUALS(iterator->_singleName, "syncdelay");
                ASSERT_EQUALS(iterator->_type, moe::Double);
                ASSERT_EQUALS(iterator->_description, "seconds between disk syncs (0=never, but not recommended)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.equal(moe::Value(60.0)));
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case upgrade:
                ASSERT_EQUALS(iterator->_singleName, "upgrade");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "upgrade db if needed");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case repair:
                ASSERT_EQUALS(iterator->_singleName, "repair");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "run repair on all dbs");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_repairPath:
                ASSERT_EQUALS(iterator->_singleName, "repairpath");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "root directory for repair files - defaults to dbpath");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case noscripting:
                ASSERT_EQUALS(iterator->_singleName, "noscripting");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable scripting engine");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case notablescan:
                ASSERT_EQUALS(iterator->_singleName, "notablescan");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "do not allow table scans");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case journal:
                ASSERT_EQUALS(iterator->_singleName, "journal");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "enable journaling");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case nojournal:
                ASSERT_EQUALS(iterator->_singleName, "nojournal");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable journaling (journaling is on by default for 64 bit)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case dur:
                ASSERT_EQUALS(iterator->_singleName, "dur");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "enable journaling");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case nodur:
                ASSERT_EQUALS(iterator->_singleName, "nodur");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable journaling");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_journal_enabled:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::Bool);
                ASSERT_EQUALS(iterator->_description, "enable journaling");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_journal_debugFlags:
                ASSERT_EQUALS(iterator->_singleName, "journalOptions");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "journal diagnostic options");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case durOptions:
                ASSERT_EQUALS(iterator->_singleName, "durOptions");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "durability diagnostic options");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case storage_journal_commitIntervalMs:
                ASSERT_EQUALS(iterator->_singleName, "journalCommitInterval");
                ASSERT_EQUALS(iterator->_type, moe::Unsigned);
                ASSERT_EQUALS(iterator->_description, "how often to group/batch commit (ms)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case replication_oplogSizeMB:
                ASSERT_EQUALS(iterator->_singleName, "oplogSize");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "size to use (in MB) for replication op log. default is 5% of disk space (i.e. large is good)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case masterSlave_master:
                ASSERT_EQUALS(iterator->_singleName, "master");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "master mode");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case masterSlave_slave:
                ASSERT_EQUALS(iterator->_singleName, "slave");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "slave mode");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case masterSlave_source:
                ASSERT_EQUALS(iterator->_singleName, "source");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "when slave: specify master as <server:port>");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case masterSlave_only:
                ASSERT_EQUALS(iterator->_singleName, "only");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "when slave: specify a single database to replicate");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case masterSlave_slavedelay:
                ASSERT_EQUALS(iterator->_singleName, "slavedelay");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "specify delay (in seconds) to be used when applying master ops to slave");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case masterSlave_autoresync:
                ASSERT_EQUALS(iterator->_singleName, "autoresync");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "automatically resync if slave data is stale");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case replication_replSet:
                ASSERT_EQUALS(iterator->_singleName, "replSet");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "arg is <setname>[/<optionalseedhostlist>]");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case replication_replSetName:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "arg is <setname>");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case replication_secondaryIndexPrefetch:
                ASSERT_EQUALS(iterator->_singleName, "replIndexPrefetch");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "specify index prefetching behavior (if secondary) [none|_id_only|all]");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case sharding_configsvr:
                ASSERT_EQUALS(iterator->_singleName, "configsvr");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "declare this is a config db of a cluster; default port 27019; default dir /data/configdb");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case sharding_shardsvr:
                ASSERT_EQUALS(iterator->_singleName, "shardsvr");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "declare this is a shard db of a cluster; default port 27018");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case sharding_clusterRole:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description,
                "Choose what role this mongod has in a sharded cluster.  Possible values are:\n"
                "    \"configsvr\": Start this node as a config server.  Starts on port 27019 by "
                "default."
                "    \"shardsvr\": Start this node as a shard server.  Starts on port 27018 by "
                "default.");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case sharding_noMoveParanoia:
                ASSERT_EQUALS(iterator->_singleName, "noMoveParanoia");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "turn off paranoid saving of data for the moveChunk command; default");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case sharding_archiveMovedChunks:
                ASSERT_EQUALS(iterator->_singleName, "moveParanoia");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "turn on paranoid saving of data during the moveChunk command (used for internal system diagnostics)");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case auditLog_log:
                ASSERT_EQUALS(iterator->_singleName, "auditLog");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "turn on auditing and specify output for log: textfile, bsonfile, syslog, console");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case auditLog_format:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Format of the audit log, if logging to a file.  (BSON/JSON)");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case auditLog_destination:
                ASSERT_EQUALS(iterator->_singleName, "");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Destination of audit log output.  (console/syslog/file)");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceYAMLConfig);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case auditLog_path:
                ASSERT_EQUALS(iterator->_singleName, "auditPath");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "full filespec for audit log file");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case auditLog_filter:
                ASSERT_EQUALS(iterator->_singleName, "auditFilter");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "filter spec to screen audit records");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case snmp_subagent:
                ASSERT_EQUALS(iterator->_singleName, "snmp-subagent");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "run snmp subagent");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case snmp_master:
                ASSERT_EQUALS(iterator->_singleName, "snmp-master");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "run snmp as master");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
#ifdef MONGO_SSL
            case net_ssl_sslOnNormalPorts:
                ASSERT_EQUALS(iterator->_singleName, "sslOnNormalPorts");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "use ssl on configured ports");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_mode:
                ASSERT_EQUALS(iterator->_singleName, "sslMode");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "set the SSL operation mode (noSSL|acceptSSL|sendAcceptSSL|sslOnly)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_PEMKeyFile:
                ASSERT_EQUALS(iterator->_singleName, "sslPEMKeyFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "PEM file for ssl");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_PEMKeyPassword:
                ASSERT_EQUALS(iterator->_singleName, "sslPEMKeyPassword");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "PEM file password");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.equal(moe::Value(std::string(""))));
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_clusterFile:
                ASSERT_EQUALS(iterator->_singleName, "sslClusterFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Key file for internal SSL authentication");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_clusterPassword:
                ASSERT_EQUALS(iterator->_singleName, "sslClusterPassword");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Internal authentication key file password");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.equal(moe::Value(std::string(""))));
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_CAFile:
                ASSERT_EQUALS(iterator->_singleName, "sslCAFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Certificate Authority file for SSL");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_CRLFile:
                ASSERT_EQUALS(iterator->_singleName, "sslCRLFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Certificate Revocation List file for SSL");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_weakCertificateValidation:
                ASSERT_EQUALS(iterator->_singleName, "sslWeakCertificateValidation");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "allow client to connect without presenting a certificate");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_allowInvalidCertificates:
                ASSERT_EQUALS(iterator->_singleName, "sslAllowInvalidCertificates");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "allow connections to servers with invalid certificates");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case net_ssl_FIPSMode:
                ASSERT_EQUALS(iterator->_singleName, "sslFIPSMode");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "activate FIPS 140-2 mode at startup");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
#endif
#ifdef _WIN32
            case install:
                ASSERT_EQUALS(iterator->_singleName, "install");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "install Windows service");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case remove:
                ASSERT_EQUALS(iterator->_singleName, "remove");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "remove Windows service");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case reinstall:
                ASSERT_EQUALS(iterator->_singleName, "reinstall");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "reinstall Windows service (equivalent to --remove followed by --install)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case processManagement_windowsService_serviceName:
                ASSERT_EQUALS(iterator->_singleName, "serviceName");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Windows service name");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case processManagement_windowsService_displayName:
                ASSERT_EQUALS(iterator->_singleName, "serviceDisplayName");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Windows service display name");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case processManagement_windowsService_description:
                ASSERT_EQUALS(iterator->_singleName, "serviceDescription");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Windows service description");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case processManagement_windowsService_serviceUser:
                ASSERT_EQUALS(iterator->_singleName, "serviceUser");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "account for service execution");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case processManagement_windowsService_servicePassword:
                ASSERT_EQUALS(iterator->_singleName, "servicePassword");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "password used to authenticate serviceUser");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
            case service:
                ASSERT_EQUALS(iterator->_singleName, "service");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "start mongodb service");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAllLegacy);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
#endif
#if defined(__linux__)
            case shutdown:
                ASSERT_EQUALS(iterator->_singleName, "shutdown");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "kill a running server (for init scripts)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
                break;
#endif
            default:
                ::mongo::StringBuilder sb;
                sb << "Found extra option: " << iterator->_dottedName <<
                      " which we did not register";
                FAIL(sb.str());
            }
        }
    }

} // namespace
