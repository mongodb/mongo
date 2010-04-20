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
#include "../../util/sock.h"

namespace mongo { 

    ReplSet *theReplSet = 0;

    ReplSet::ReplSet(string cfgString) {
        const char *p = cfgString.c_str();
        const char *q = strchr(p, '/');
        uassert(13093, "bad --replSet config string format is: <setname>/<seedhost1>,<seedhost2>[,...]", q != 0 && p != q);
        _name = string(p, q-p);
        log() << "replSet: " << cfgString << endl;

        set<HostAndPort> temp;
        vector<HostAndPort> *seeds = new vector<HostAndPort>;
        while( 1 ) {
            p = q + 1;
            q = strchr(p, ',');
            if( q == 0 ) q = strchr(p,0);
            uassert(13094, "bad --replSet config string", p != q);
            const char *colon = strchr(p, ':');
            HostAndPort m;
            if( colon && colon < q ) {
                int port = atoi(colon+1);
                uassert(13095, "bad --replSet port #", port > 0);
                m = HostAndPort(string(p,colon-p),port);
            }
            else { 
                // no port specified.
                m = HostAndPort(string(p,q-p), cmdLine.port);
            }
            uassert(13096, "bad --replSet config string - dups?", temp.count(m) == 0 ); // these uasserts leak seeds but that's ok
            temp.insert(m);

            uassert(13101, "can't use localhost in replset host list", !m.isLocalHost());

            if( m.isSelf() )
                log() << "replSet: ignoring seed " << m.toString() << " (=self)" << endl;
            else
                seeds->push_back(m);
            if( *q == 0 )
                break;
        }

        _seeds = seeds;
        for( vector<HostAndPort>::iterator i = seeds->begin(); i != seeds->end(); i++ )
            addMemberIfMissing(*i);

        startHealthThreads();
    }

    void ReplSet::addMemberIfMissing(const HostAndPort& h) { 
        MemberInfo *m = _members.head();
        while( m ) {
            if( h.host() == m->host && h.port() == m->port )
                return;
            m = m->next();
        }
        MemberInfo *nm = new MemberInfo(h.host(), h.port());
        _members.push(nm);
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
