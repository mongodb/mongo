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
#include "rs.h"
#include "../client.h"

namespace mongo {

    enum { 
        NOPRIMARY = -2,
        SELFPRIMARY = -1
    };

    /* check members OTHER THAN US to see if they think they are primary */
    const Member * Manager::findOtherPrimary() { 
        Member *m = rs->head();
        Member *p = 0;
        while( m ) {
            if( m->state() == RS_PRIMARY ) {
                if( p ) throw "twomasters"; // our polling is asynchronous, so this is often ok.
                p = m;
            }
            m = m->next();
        }
        if( p ) 
            noteARemoteIsPrimary(p);
        return p;
    }

    Manager::Manager(ReplSetImpl *_rs) : 
    task::Server("rs Manager"), rs(_rs), busyWithElectSelf(false), _primary(NOPRIMARY)
    { 
    }
 
    void Manager::starting() { 
        Client::initThread("rs Manager");
    }

    void Manager::noteARemoteIsPrimary(const Member *m) { 
        rs->_currentPrimary = m;
        rs->_self->lhb() = "";
        rs->_myState = RS_RECOVERING;
    }

    /** called as the health threads get new results */
    void Manager::msgCheckNewState() {
        {
            RSBase::lock lk(rs);

            if( busyWithElectSelf ) return;

            const Member *p = rs->currentPrimary();
            const Member *p2;
            try { p2 = findOtherPrimary(); }
            catch(string s) { 
                /* two other nodes think they are primary (asynchronously polled) -- wait for things to settle down. */
                log() << "replSet warning DIAG TODO 2primary" << s << rsLog;
                return;
            }

            if( p == p2 && p ) {
                return;
            }

            if( p2 ) { 
                /* someone else thinks they are primary. */
                if( p == p2 ) { // already match 
                    return;
                }
                if( p == 0 ) {
                    noteARemoteIsPrimary(p2); 
                    return;
                }
                if( p != rs->_self ) {
                    noteARemoteIsPrimary(p2); 
                    return;
                }
                /* we thought we were primary, yet now someone else thinks they are. */
                if( !rs->elect.aMajoritySeemsToBeUp() ) {
                    noteARemoteIsPrimary(p2); 
                    return;
                }
                /* ignore for now, keep thinking we are master */
                return;
            }

            if( p ) { 
                /* we are already primary, and nothing significant out there has changed. */
                /* todo: if !aMajoritySeemsToBeUp, relinquish */
                assert( p == rs->_self );
                return;
            }

            /* no one seems to be primary.  shall we try to elect ourself? */
            if( !rs->elect.aMajoritySeemsToBeUp() ) { 
                rs->_self->lhb() = "can't see a majority, won't consider electing self";
                return;
            }

            rs->_self->lhb() = "";
            busyWithElectSelf = true; // don't try to do further elections & such while we are already working on one.
        }
        try { 
            rs->elect.electSelf(); 
        }
        catch(RetryAfterSleepException&) {
            /* we want to process new inbounds before trying this again.  so we just put a checkNewstate in the queue for eval later. */
            requeue();
        }
        catch(...) { 
            log() << "replSet error unexpected assertion in rs manager" << rsLog; 
        }
        busyWithElectSelf = false;
    }

}
