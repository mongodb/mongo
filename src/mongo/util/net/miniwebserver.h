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

#include "../../pch.h"
#include "message.h"
#include "message_port.h"
#include "listen.h"
#include "../../db/jsobj.h"

namespace mongo {

    class MiniWebServer : public Listener {
    public:
        MiniWebServer(const string& name, const string &ip, int _port);
        virtual ~MiniWebServer() {}

        virtual void doRequest(
            const char *rq, // the full request
            string url,
            // set these and return them:
            string& responseMsg,
            int& responseCode,
            vector<string>& headers, // if completely empty, content-type: text/html will be added
            const SockAddr &from
        ) = 0;

        // --- static helpers ----

        static void parseParams( BSONObj & params , string query );

        static string parseURL( const char * buf );
        static string parseMethod( const char * headers );
        static string getHeader( const char * headers , string name );
        static const char *body( const char *buf );

        static string urlDecode(const char* s);
        static string urlDecode(string s) {return urlDecode(s.c_str());}

    private:
        void accepted(boost::shared_ptr<Socket> psocket);
        static bool fullReceive( const char *buf );
    };

} // namespace mongo
