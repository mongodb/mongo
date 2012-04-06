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

namespace mongo {

    class Scope;
    
    namespace shellUtils {

        // Scoped management of mongo program instances.  Simple implementation:
        // destructor kills all mongod instances created by the shell.
        struct MongoProgramScope {
            MongoProgramScope() {} // Avoid 'unused variable' warning.
            ~MongoProgramScope();
        };
        void KillMongoProgramInstances();
        
        void goingAwaySoon();
        void installShellUtilsLauncher( Scope& scope );
        
        /** Helper class for launching a program and logging its output. */
        class ProgramRunner {
        public:
            /** @param args The program's arguments, including the program name. */
            ProgramRunner( const BSONObj &args );
            /** Launch the program. */
            void start();                        
            /** Continuously read the program's output, generally from a new thread. */
            void operator()();
            pid_t pid() const { return pid_; }
            int port() const { return port_; }

        private:
            boost::filesystem::path find(string prog);
            void launch_process(int child_stdout);
            
            vector<string> argv_;
            int port_;
            int pipe_;
            pid_t pid_;
        };
    }
}
