// dbwebserver.cpp

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

#include "stdafx.h"
#include "../util/miniwebserver.h"
#include "db.h"

extern int port;

class DbWebServer : public MiniWebServer { 
public:
    void doLockedStuff(stringstream& ss) { }

    virtual void doRequest(
        const char *rq, // the full request
        string url,
        // set these and return them:
        string& responseMsg, 
        int& responseCode,
        vector<string>& headers // if completely empty, content-type: text/html will be added
        ) 
    {
        responseCode = 200;
        stringstream ss;
        ss << "<html><head><title>db</title></head><body>db<p>\n<pre>";

        ss << "port:     " << port << '\n';
        ss << "dblocked: " << dbLocked << " (initial)\n";

        int n = 2000;
        while( 1 ) {
            if( !dbLocked ) { 
                doLockedStuff(ss);
                break;
            }
            sleepmillis(1);
            if( --n < 0 ) {
                ss << "\ntimed out getting dblock\n";
                break;
            }
        }

        ss << "</pre></body></html>";
        responseMsg = ss.str();
    }
};

void webServerThread() {
    DbWebServer mini;
    if( mini.init(port+1000) )
        mini.run();
}
