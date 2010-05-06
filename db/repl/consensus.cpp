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

    class E : public BackgroundJob { 
        void run() { 
            log() << "not done" << endl;
        }
    public:
        ReplSet::Member *_m;
    };

    bool ReplSet::aMajoritySeemsToBeUp() const {
        Member *m = head();
        unsigned vTot = 0;
        unsigned vUp = 0;
        do {
            vTot += m->config().votes;
            if( m->up() )
                vTot += m->config().votes;
            m = m->next();
        } while( m );
        return vUp * 2 > vTot;
    }

    typedef shared_ptr<E> eptr;
    void ReplSet::electSelf() {
        list<BackgroundJob*> _jobs;
        list<eptr> jobs;
        for( Member *m = head(); m; m=m->next() ) { 
            eptr e( new E() );
            e->_m = m;
            jobs.push_back(e);
            _jobs.push_back(e.get());
            e->go();
        }
        BackgroundJob::wait(_jobs);
    }

}
