// mongo/shell/shell_utils_launcher.h
/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */


#pragma once

#include <boost/filesystem/convenience.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

#include "mongo/bson/bsonobj.h"
#include "mongo/platform/process_id.h"

namespace mongo {

    class Scope;
    
    namespace shell_utils {

        // Scoped management of mongo program instances.  Simple implementation:
        // destructor kills all mongod instances created by the shell.
        struct MongoProgramScope {
            MongoProgramScope() {} // Avoid 'unused variable' warning.
            ~MongoProgramScope();
        };
        void KillMongoProgramInstances();
        
        void goingAwaySoon();
        void installShellUtilsLauncher( Scope& scope );
        
        /** Record log lines from concurrent programs.  All public members are thread safe. */
        class ProgramOutputMultiplexer {
        public:
            void appendLine( int port, ProcessId pid, const char *line );
            /** @return up to 100000 characters of the most recent log output. */
            std::string str() const;
            void clear();
        private:
            std::stringstream _buffer;
        };

        /**
         * A registry of spawned programs that are identified by a bound port or else a system pid.
         * All public member functions are thread safe.
         *
         * TODO: Clean this up to make the semantics more consistent between pids and ports
         */
        class ProgramRegistry {
        public:

            bool isPortRegistered( int port ) const;
            /** @return pid for a registered port. */
            ProcessId pidForPort( int port ) const;
            /** @return port (-1 if doesn't exist) for a registered pid. */
            int portForPid( ProcessId pid ) const;
            /** Register an unregistered port. */
            void registerPort( int port, ProcessId pid, int output );
            void deletePort( int port );
            void getRegisteredPorts( std::vector<int> &ports );

            bool isPidRegistered( ProcessId pid ) const;
            /** Register an unregistered pid. */
            void registerPid( ProcessId pid, int output );
            void deletePid( ProcessId pid );
            void getRegisteredPids( vector<ProcessId> &pids );

        private:
            std::map<int,std::pair<ProcessId,int> > _ports;
            std::map<ProcessId,int> _pids;
            mutable boost::recursive_mutex _mutex;

#ifdef _WIN32
        public:
            std::map<ProcessId,HANDLE> _handles;
#endif
        };
        
        /** Helper class for launching a program and logging its output. */
        class ProgramRunner {
        public:
            /** @param args The program's arguments, including the program name. */
            ProgramRunner( const BSONObj &args );
            /** Launch the program. */
            void start();                        
            /** Continuously read the program's output, generally from a special purpose thread. */
            void operator()();
            ProcessId pid() const { return _pid; }
            int port() const { return _port; }

        private:
            boost::filesystem::path findProgram( const string &prog );
            void launchProcess( int child_stdout );
            
            std::vector<std::string> _argv;
            int _port;
            int _pipe;
            ProcessId _pid;
        };
    }
}
