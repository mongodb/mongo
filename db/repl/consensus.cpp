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

    bool ReplSet::Consensus::aMajoritySeemsToBeUp() const {
        Member *m =rs.head();
        unsigned vTot = 0, vUp = 0;
        for( Member *m = rs.head(); m; m=m->next() ) { 
            vTot += m->config().votes;
            vUp += m->up() ? m->config().votes : 0;
        }
        return vUp * 2 > vTot;
    }

        class E : public BackgroundJob { 
            void run() { 
                log() << "not done" << endl;
                try { 
                    ScopedConn c(m->fullName());
                    //c.runCommand(
                }
                catch(DBException&) { 
                }
            }
        public:
            ReplSet::Member *m;
        };
        typedef shared_ptr<E> eptr;

    void ReplSet::Consensus::electSelf() {
        list<eptr> jobs;
        list<BackgroundJob*> _jobs;
        for( Member *m = rs.head(); m; m=m->next() ) if( m->up() ) {
            E *e = new E();
            e->m = m;
            jobs.push_back(eptr(e)); _jobs.push_back(e);
            e->go();
        }
        BackgroundJob::wait(_jobs,5);
    }

}
