/* @file manager.cpp 
*/

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "replset.h"

namespace mongo {

    enum { 
        NOPRIMARY = -2,
        SELFPRIMARY = -1
    };

    /* check members OTHER THAN US to see if they think they are primary */
    const ReplSet::Member * ReplSet::Manager::findOtherPrimary() { 
        Member *m = _rs->head();
        Member *p = 0;
        while( m ) {
            if( m->state() == PRIMARY ) {
                if( p ) throw "twomasters"; // our polling is asynchronous, so this is often ok.
                p = m;
            }
            m = m->next();
        }
        return p;
    }

    ReplSet::Manager::Manager(ReplSet *rs) : _rs(rs), _primary(NOPRIMARY)
    { 
    }

    void ReplSet::Manager::noteARemoteIsPrimary(const Member *m) { 
        _rs->_self->_lastHeartbeatErrMsg.set("finish code #1");
        log() << "replSet finish notearemoteisprimary " << m->fullName() << rsLog;
    }

    /** called as the health threads get new results */
    void ReplSet::Manager::checkNewState() {
        static mutex m;
        scoped_lock lk(m);

        //log() << "replSet checkNewState " << rsLog;

        const Member *p = _rs->currentPrimary();
        const Member *p2 = findOtherPrimary();
        try { p2 = findOtherPrimary(); }
        catch(string s) { 
            log() << "replSet warning DIAG TODO " << s << rsLog;
            return;
        }

        if( p == p2 && p ) return;

        if( p2 ) { 
            /* someone else thinks they are primary. */
            if( p == p2 ) 
                return;
            if( p == 0 )
                noteARemoteIsPrimary(p2); return;
            if( p != _rs->_self )
                noteARemoteIsPrimary(p2); return;
            /* we thought we were primary, yet now someone else thinks they are. */
            if( !_rs->elect.aMajoritySeemsToBeUp() )
                noteARemoteIsPrimary(p2); return;
            /* ignore for now, keep thinking we are master */
            return;
        }

        if( p ) { 
            /* we are already primary, and nothing significant out there has changed. */\
            /* todo: if !aMajoritySeemsToBeUp, relinquish */
            assert( p == _rs->_self );
            return;
        }

        /* no one seems to be primary.  shall we try to elect ourself? */
        if( !_rs->elect.aMajoritySeemsToBeUp() ) { 
            _rs->_self->_lastHeartbeatErrMsg.set("can't see a majority, won't elect self");
            return;
        }

        log() << "replSet todo elect self as primary primary" << rsLog;
        _rs->_self->_lastHeartbeatErrMsg.set("todo code #2");
    }


}
