/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/net/listen.h"

namespace mongo {

    namespace optionenvironment {
        class OptionSection;
        class Environment;
    } // namespace optionenvironment

    /* command line options
    */
    /* concurrency: OK/READ */
    struct CmdLine {

        CmdLine();

        std::string binaryName;     // mongod or mongos
        std::string cwd;            // cwd of when process started

        // this is suboptimal as someone could rename a binary.  todo...
        bool isMongos() const { return binaryName == "mongos"; }

        int port;              // --port
        enum {
            DefaultDBPort = 27017,
            ConfigServerPort = 27019,
            ShardServerPort = 27018
        };
        bool isDefaultPort() const { return port == DefaultDBPort; }

        std::string bind_ip;        // --bind_ip
        bool rest;             // --rest
        bool jsonp;            // --jsonp

        std::string _replSet;       // --replSet[/<seedlist>]
        std::string ourSetName() const {
            std::string setname;
            size_t sl = _replSet.find('/');
            if( sl == std::string::npos )
                return _replSet;
            return _replSet.substr(0, sl);
        }
        bool usingReplSets() const { return !_replSet.empty(); }

        std::string rsIndexPrefetch;// --indexPrefetch
        bool indexBuildRetry;  // --noIndexBuildRetry

        // for master/slave replication
        std::string source;    // --source
        std::string only;      // --only

        bool quiet;            // --quiet
        bool noTableScan;      // --notablescan no table scans allowed
        bool prealloc;         // --noprealloc no preallocation of data files
        bool preallocj;        // --nopreallocj no preallocation of journal files
        bool smallfiles;       // --smallfiles allocate smaller data files

        bool configsvr;        // --configsvr

        bool quota;            // --quota
        int quotaFiles;        // --quotaFiles
        bool cpu;              // --cpu show cpu time periodically

        bool dur;                       // --dur durability (now --journal)
        unsigned journalCommitInterval; // group/batch commit interval ms

        /** --durOptions 7      dump journal and terminate without doing anything further
            --durOptions 4      recover and terminate without listening
        */
        enum { // bits to be ORed
            DurDumpJournal = 1,   // dump diagnostics on the journal during recovery
            DurScanOnly = 2,      // don't do any real work, just scan and dump if dump specified
            DurRecoverOnly = 4,   // terminate after recovery step
            DurParanoid = 8,      // paranoid mode enables extra checks
            DurAlwaysCommit = 16, // do a group commit every time the writelock is released
            DurAlwaysRemap = 32,  // remap the private view after every group commit (may lag to the next write lock acquisition, but will do all files then)
            DurNoCheckSpace = 64  // don't check that there is enough room for journal files before startup (for diskfull tests)
        };
        int durOptions;          // --durOptions <n> for debugging

        bool objcheck;         // --objcheck

        long long oplogSize;   // --oplogSize
        int defaultProfile;    // --profile
        int slowMS;            // --time in ms that is "slow"
        int defaultLocalThresholdMillis;    // --localThreshold in ms to consider a node local
        int pretouch;          // --pretouch for replication application (experimental)
        bool moveParanoia;     // for move chunk paranoia
        double syncdelay;      // seconds between fsyncs

        bool noUnixSocket;     // --nounixsocket
        bool doFork;           // --fork
        std::string socket;    // UNIX domain socket directory

        int maxConns;          // Maximum number of simultaneous open connections.

        std::string keyFile;   // Path to keyfile, or empty if none.
        std::string pidFile;   // Path to pid file, or empty if none.

        std::string logpath;   // Path to log file, if logging to a file; otherwise, empty.
        bool logAppend;        // True if logging to a file in append mode.
        bool logWithSyslog;    // True if logging to syslog; must not be set if logpath is set.
        std::string clusterAuthMode; // Cluster authentication mode

        bool isHttpInterfaceEnabled; // True if the dbwebserver should be enabled.

#ifndef _WIN32
        ProcessId parentProc;      // --fork pid of initial process
        ProcessId leaderProc;      // --fork pid of leader process
#endif
#ifdef MONGO_SSL
        bool sslOnNormalPorts;      // --sslOnNormalPorts
        std::string sslPEMKeyFile;       // --sslPEMKeyFile
        std::string sslPEMKeyPassword;   // --sslPEMKeyPassword
        std::string sslClusterFile;       // --sslInternalKeyFile
        std::string sslClusterPassword;   // --sslInternalKeyPassword
        std::string sslCAFile;      // --sslCAFile
        std::string sslCRLFile;     // --sslCRLFile
        bool sslWeakCertificateValidation; // --sslWeakCertificateValidation
        bool sslFIPSMode; // --sslFIPSMode
#endif

        /**
         * Switches to enable experimental (unsupported) features.
         */
        struct ExperimentalFeatures {
            ExperimentalFeatures()
                : indexStatsCmdEnabled(false)
                , storageDetailsCmdEnabled(false)
            {}
            bool indexStatsCmdEnabled; // -- enableExperimentalIndexStatsCmd
            bool storageDetailsCmdEnabled; // -- enableExperimentalStorageDetailsCmd
        } experimental;

        static void launchOk();

        /**
         * @return true if should run program, false if should exit
         */
        static Status store( const std::vector<std::string>& argv,
                             optionenvironment::OptionSection& options,
                             optionenvironment::Environment& output );

        /**
         * Blot out sensitive fields in the argv array.
         */
        static void censor(int argc, char** argv);
        static void censor(std::vector<std::string>* args);

        static BSONArray getArgvArray();
        static BSONObj getParsedOpts();

        static Status setupBinaryName(const std::vector<std::string>& argv);
        static Status setupCwd();
        static Status setArgvArray(const std::vector<std::string>& argv);
        static Status setParsedOpts(optionenvironment::Environment& params);

        time_t started;
    };

    // todo move to cmdline.cpp?
    inline CmdLine::CmdLine() :
        port(DefaultDBPort), rest(false), jsonp(false), indexBuildRetry(true), quiet(false),
        noTableScan(false), prealloc(true), preallocj(true), smallfiles(sizeof(int*) == 4),
        configsvr(false), quota(false), quotaFiles(8), cpu(false),
        durOptions(0), objcheck(true), oplogSize(0), defaultProfile(0),
        slowMS(100), defaultLocalThresholdMillis(15), pretouch(0), moveParanoia( true ),
        syncdelay(60), noUnixSocket(false), doFork(0), socket("/tmp"), maxConns(DEFAULT_MAX_CONN),
        logAppend(false), logWithSyslog(false), isHttpInterfaceEnabled(false)
    {
        started = time(0);

        journalCommitInterval = 0; // 0 means use default
        dur = false;
#if defined(_DURABLEDEFAULTON)
        dur = true;
#endif
        if( sizeof(void*) == 8 )
            dur = true;
#if defined(_DURABLEDEFAULTOFF)
        dur = false;
#endif

#ifdef MONGO_SSL
        sslOnNormalPorts = false;
#endif
    }

    extern CmdLine cmdLine;

    void printCommandLineOpts();
}

