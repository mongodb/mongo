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
#include "../cmdline.h"
#include "replset.h"

namespace mongo { 

    ReplSet *theReplSet = 0;

    ReplSet::ReplSet(string cfgString) {
        const char *p = cfgString.c_str();
        const char *q = strchr(p, '/');
        uassert(13093, "bad --replset config string format is: <setname>/<seedhost1>,<seedhost2>[,...]", q != 0 && p != q);
        name = string(p, q-p);

        set<RemoteServer> temp;
        while( 1 ) {
            p = q + 1;
            q = strchr(p, ',');
            if( q == 0 ) q = strchr(p,0);
            uassert(13094, "bad --replset config string", p != q);
            const char *colon = strchr(p, ':');
            RemoteServer m;
            if( colon && colon < q ) {
                int port = atoi(colon+1);
                uassert(13095, "bad --replset port #", port > 0);
                m = RemoteServer(string(p,colon-p),port);
            }
            else { 
                // no port specified.
                m = RemoteServer(string(p,q-p));
            }
            uassert(13096, "bad --replset config string - dups?", temp.count(m) == 0 );
            temp.insert(m);
            _seeds.push_back(m);
            if( *q == 0 )
                break;
        }

        boost::thread t(ReplSet::healthThread);
    }

    /* called at initialization */
    bool startReplSets() {
        assert( theReplSet == 0 );
        if( cmdLine.replSet.empty() )
            return false;
        theReplSet = new ReplSet(cmdLine.replSet);
        return false; 
    }

}
