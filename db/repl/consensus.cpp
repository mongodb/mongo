/**
*    Copyright (C) 2010 10gen Inc.
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

#include "pch.h"
#include "replset.h"
#include "multicmd.h"

namespace mongo { 

    int ReplSet::Consensus::totalVotes() const { 
        static int complain = 0;
        int vTot = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) 
            vTot += m->config().votes;
        if( vTot % 2 == 0 && vTot && complain++ == 0 )
            log() << "replSet warning: total number of votes is even - considering giving one member an extra vote" << rsLog;
        return vTot;
    }

    bool ReplSet::Consensus::aMajoritySeemsToBeUp() const {
        int vUp = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) 
            vUp += m->hbinfo().up() ? m->config().votes : 0;
        return vUp * 2 > totalVotes();
    }

    static const int VETO = -10000;

    void ReplSet::Consensus::_electSelf() {
        ReplSet::Member& me = *rs._self;
        BSONObj electCmd = BSON(
               "replSetElect" << 1 <<
               "set" << rs.name() << 
               "who" << me.fullName() << 
               "whoid" << me.hbinfo().id() << 
               "cfgver" << rs._cfg->version
            );

        list<Target> L;
        for( Member *m = rs.head(); m; m=m->next() )
            if( m->hbinfo().up() )
                L.push_back( Target(m->fullName()) );

        time_t start = time(0);
        multiCommand(electCmd, L);

        int tally = me.config().votes; // we vote yes. ?
        for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
            cout << "TEMP electres: " << i->result.toString() << endl;
            if( i->ok ) {
                int v = i->result["vote"].Int();
                tally += v;
            }
        }
        if( tally*2 > totalVotes() ) {
            if( time(0) - start > 30 ) {
                // defensive; should never happen as we have timeouts on connection and operation for our conn
                log() << "replSet too much time passed during election, ignoring result" << rsLog;
            }
            /* succeeded. */
            rs._myState = PRIMARY;
            log() << "replSet elected self as primary" << rsLog;
            return;
        } 
    }

    void ReplSet::Consensus::electSelf() {
        try { _electSelf(); } catch(...) { }
    }

}
