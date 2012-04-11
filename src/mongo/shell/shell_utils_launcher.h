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

#ifdef _WIN32
typedef int pid_t;
#endif

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
            void appendLine( int port, int pid, const char *line );
            /** @return up to 100000 characters of the most recent log output. */
            string str() const;
            void clear();
        private:
            stringstream _buffer;
        };

        /**
         * A registry of spawned programs that are identified by a bound port or else a system pid.
         * All public member functions are thread safe.
         */
        class ProgramRegistry {
        public:

            bool isPortRegistered( int port ) const;
            /** @return pid for a registered port. */
            pid_t pidForPort( int port ) const;
            /** Register an unregistered port. */
            void registerPort( int port, pid_t pid, int output );
            void deletePort( int port );
            void getRegisteredPorts( vector<int> &ports );

            bool isPidRegistered( pid_t pid ) const;
            /** Register an unregistered pid. */
            void registerPid( pid_t pid, int output );
            void deletePid( pid_t pid );
            void getRegisteredPids( vector<pid_t> &pids );

        private:
            map<int,pair<pid_t,int> > _ports;
            map<pid_t,int> _pids;
            mutable boost::recursive_mutex _mutex;

#ifdef _WIN32
        public:
            map<pid_t,HANDLE> _handles;
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
            pid_t pid() const { return _pid; }
            int port() const { return _port; }

        private:
            boost::filesystem::path findProgram( const string &prog );
            void launchProcess( int child_stdout );
            
            vector<string> _argv;
            int _port;
            int _pipe;
            pid_t _pid;
        };
    }
}
