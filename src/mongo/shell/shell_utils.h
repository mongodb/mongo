// mongo/shell/shell_utils.h
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

#include "mongo/db/jsobj.h"

namespace mongo {

    class Scope;
    class DBClientWithCommands;
    
    namespace shell_utils {

        extern std::string _dbConnect;
        extern std::string _dbAuth;
        extern bool _nokillop;

        void RecordMyLocation( const char *_argv0 );
        void installShellUtils( Scope& scope );

        void initScope( Scope &scope );
        void onConnect( DBClientWithCommands &c );

        const char* getUserDir();
        
        BSONElement singleArg(const BSONObj& args);
        extern const BSONObj undefinedReturn;
        
        /** Prompt for confirmation from cin. */
        class Prompter {
        public:
            Prompter( const string &prompt );
            /** @return prompted confirmation or cached confirmation. */
            bool confirm();
        private:
            const string _prompt;
            bool _confirmed;
        };

        /** Registry of server connections. */
        class ConnectionRegistry {
        public:
            ConnectionRegistry();
            void registerConnection( DBClientWithCommands &client );
            void killOperationsOnAllConnections( bool withPrompt ) const;
        private:
            map<string,set<string> > _connectionUris;
            mutable mongo::mutex _mutex;
        };
        
        extern ConnectionRegistry connectionRegistry;
    }
}
