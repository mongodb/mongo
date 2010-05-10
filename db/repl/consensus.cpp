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
#include "connections.h"
#include "../../util/background.h"

namespace mongo { 

    int ReplSet::Consensus::totalVotes() const { 
        static int complain = 0;
        Member *m =rs.head();
        int vTot = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) { 
            vTot += m->config().votes;
        }
        if( vTot % 2 == 0 && vTot && complain++ == 0 )
            log() << "replSet warning: total number of votes is even - considering giving one member an extra vote" << rsLog;
        return vTot;
    }

    bool ReplSet::Consensus::aMajoritySeemsToBeUp() const {
        Member *m =rs.head();
        int vUp = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) { 
            vUp += m->up() ? m->config().votes : 0;
        }
        return vUp * 2 > totalVotes();
    }

    static BSONObj electCmd;
    
    class E : public BackgroundJob { 
        void run() { 
            ok = 0;
            log() << "not done" << rsLog;
            try { 
                ScopedConn c(m->fullName());
                if( c->runCommand("admin", electCmd, result) )
                    ok++;
            }
            catch(DBException&) { 
                DEV log() << "replSet dev caught dbexception on electCmd" << rsLog;
            }
        }
    public:
        BSONObj result;
        int ok;
        ReplSet::Member *m;
    };
    typedef shared_ptr<E> eptr;

    static const int VETO = -10000;

    bool ReplSet::Consensus::electSelf() {
        ReplSet::Member& me = *rs._self;
        electCmd = BSON(
               "replSetElect" << 1 <<
               "set" << rs.getName() << 
               "who" << me.fullName() << 
               "whoid" << me._id << 
               "cfgver" << rs._cfg->version
            );
        list<eptr> jobs;
        list<BackgroundJob*> _jobs;
        for( Member *m = rs.head(); m; m=m->next() ) if( m->up() ) {
            E *e = new E();
            e->m = m;
            jobs.push_back(eptr(e)); _jobs.push_back(e);
        }

        time_t start = time(0);
        BackgroundJob::go(_jobs);
        BackgroundJob::wait(_jobs,5);

        int tally = me.config().votes; // me votes yes.
        for( list<eptr>::iterator i = jobs.begin(); i != jobs.end(); i++ ) {
            if( (*i)->ok ) {
                int v = (*i)->result["vote"].Int();
            }
        }
        if( tally*2 > totalVotes() ) {
            if( time(0) - start > 30 ) {
                // defensive; should never happen as we have timeouts on connection and operation for our conn
                log() << "replSet too much time passed during election, ignoring result" << rsLog;
                return false;
            }
            return true;
        } 
        return false;
    }

}
