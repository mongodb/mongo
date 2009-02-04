// ConfigServer.cpp

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
#include "configserver.h"
#include "server.h"

namespace mongo {

    ConfigServer::ConfigServer() {
        _conn = 0;
    }

    ConfigServer::~ConfigServer() {
        if ( _conn )
            delete _conn;
        _conn = 0; // defensive
    }

    bool ConfigServer::init( vector<string> configHosts , bool infer ){
        string hn = getHostName();
        if ( hn.empty() ) {
            sleepsecs(5);
            exit(16);
        }
        ourHostname = hn;

        char buf[256];
        strcpy(buf, hn.c_str());

        if ( configHosts.empty() ) {
            char *p = strchr(buf, '-');
            if ( p )
                p = strchr(p+1, '-');
            if ( !p ) {
                log() << "can't parse server's hostname, expect <city>-<locname>-n<nodenum>, got: " << buf << endl;
                sleepsecs(5);
                exit(17);
            }
            p[1] = 0;
        }

        string left, right; // with :port#
        string hostLeft, hostRight;

        if ( configHosts.empty() ) {
            if ( ! infer ) {
                out() << "--griddb or --infer required\n";
                exit(7);
            }
            stringstream sl, sr;
            sl << buf << "grid-l";
            sr << buf << "grid-r";
            hostLeft = sl.str();
            hostRight = sr.str();
            sl << ":" << Port;
            sr << ":" << Port;
            left = sl.str();
            right = sr.str();
        }
        else {
            stringstream sl, sr;
            sl << configHosts[0];
            hostLeft = sl.str();
            sl << ":" << Port;
            left = sl.str();

            if ( configHosts.size() > 1 ) {
                sr << configHosts[1];
                hostRight = sr.str();
                sr << ":" << Port;
                right = sr.str();
            }
        }


        if ( !isdigit(left[0]) )
            /* this loop is not really necessary, we we print out if we can't connect
               but it gives much prettier error msg this way if the config is totally
               wrong so worthwhile.
               */
            while ( 1 ) {
                if ( hostbyname(hostLeft.c_str()).empty() ) {
                    log() << "can't resolve DNS for " << hostLeft << ", sleeping and then trying again" << endl;
                    sleepsecs(15);
                    continue;
                }
                if ( !hostRight.empty() && hostbyname(hostRight.c_str()).empty() ) {
                    log() << "can't resolve DNS for " << hostRight << ", sleeping and then trying again" << endl;
                    sleepsecs(15);
                    continue;
                }
                break;
            }

        Nullstream& l = log();
        l << "connecting to griddb ";

        bool ok;
        if ( !hostRight.empty() ) {
            // connect in paired mode
            l << "L:" << left << " R:" << right << "...";
            l.flush();
            DBClientPaired *dbp = new DBClientPaired();
            _conn = dbp;
            ok = dbp->connect(left.c_str(),right.c_str());
        }
        else {
            l << left << "...";
            l.flush();
            DBClientConnection *dcc = new DBClientConnection(/*autoreconnect=*/true);
            _conn = dcc;
            string errmsg;
            ok = dcc->connect(left.c_str(), errmsg);
        }
        
        return ok;
    }

    ConfigServer configServer;

} // namespace mongo
