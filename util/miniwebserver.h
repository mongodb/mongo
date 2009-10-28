// miniwebserver.h

/*    Copyright 2009 10gen Inc.
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

#include "message.h"

namespace mongo {

    class MiniWebServer {
    public:
        MiniWebServer();
        virtual ~MiniWebServer() {}

        bool init(const string &ip, int _port);
        void run();

        virtual void doRequest(
            const char *rq, // the full request
            string url,
            // set these and return them:
            string& responseMsg,
            int& responseCode,
            vector<string>& headers, // if completely empty, content-type: text/html will be added
            const SockAddr &from
        ) = 0;

        int socket() const { return sock; }
        
    protected:
        string parseURL( const char * buf );
        string parseMethod( const char * headers );
        string getHeader( const char * headers , string name );
        void parseParams( map<string,string> & params , string query );
        static const char *body( const char *buf );

    private:
        void accepted(int s, const SockAddr &from);
        static bool fullReceive( const char *buf );

        int port;
        int sock;
    };

} // namespace mongo
