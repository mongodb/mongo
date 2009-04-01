// miniwebserver.h

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
*/

#pragma once

#include "message.h"

namespace mongo {

    class MiniWebServer {
    public:
        MiniWebServer();
        virtual ~MiniWebServer() {}

        bool init(int _port);
        void run();

        virtual void doRequest(
            const char *rq, // the full request
            string url,
            // set these and return them:
            string& responseMsg,
            int& responseCode,
            vector<string>& headers // if completely empty, content-type: text/html will be added
        ) = 0;

        int socket() const { return sock; }
        
    protected:
        string parseURL( const char * buf );
        string parseMethod( const char * headers );
        string getHeader( const char * headers , string name );
        void parseParams( map<string,string> & params , string query );
        static const char *body( const char *buf );

    private:
        void accepted(int s);
        static bool fullReceive( const char *buf );

        int port;
        int sock;
    };

} // namespace mongo
