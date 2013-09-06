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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/client.h"

namespace mongo {

    enum {
        NOPRIMARY = -2,
        SELFPRIMARY = -1
    };

    /* check members OTHER THAN US to see if they think they are primary */
    const Member * Manager::findOtherPrimary(bool& two) {
        two = false;
        Member *m = rs->head();
        Member *p = 0;
        while( m ) {
            DEV verify( m != rs->_self );
            if( m->state().primary() && m->hbinfo().up() ) {
                if( p ) {
                    two = true;
                    return 0;
                }
                p = m;
            }
            m = m->next();
        }
        if( p )
            noteARemoteIsPrimary(p);
        return p;
    }

    Manager::Manager(ReplSetImpl *_rs) :
        task::Server("rsMgr"), rs(_rs), busyWithElectSelf(false), _primary(NOPRIMARY) {
    }

    Manager::~Manager() {
        /* we don't destroy the replset object we sit in; however, the destructor could have thrown on init.
           the log message below is just a reminder to come back one day and review this code more, and to
           make it cleaner.
           */
        log() << "info: ~Manager called" << rsLog;
        rs->mgr = 0;
    }

    void Manager::starting() {
        Client::initThread("rsMgr");
        replLocalAuth();
    }

    void Manager::noteARemoteIsPrimary(const Member *m) {
        if( rs->box.getPrimary() == m )
            return;
        rs->_self->lhb() = "";

        // this is what actually puts arbiters into ARBITER state
        if( rs->iAmArbiterOnly() ) {
            rs->box.set(MemberState::RS_ARBITER, m);
            return;
        }

        if (rs->box.getState().primary()) {
            // make sure exactly one primary steps down
            if (rs->selfId() < m->id()) {
                return;
            }

            rs->relinquish();
        }

        rs->box.noteRemoteIsPrimary(m);
    }

    void Manager::checkElectableSet() {
        unsigned otherOp = rs->lastOtherOpTime().getSecs();
        
        // make sure the electable set is up-to-date
        if (rs->elect.aMajoritySeemsToBeUp() &&
            rs->iAmPotentiallyHot() &&
            (otherOp == 0 || rs->lastOpTimeWritten.getSecs() >= otherOp - 10)) {
            theReplSet->addToElectable(rs->selfId());
        }
        else {
            theReplSet->rmFromElectable(rs->selfId());
        }

        // check if we should ask the primary (possibly ourselves) to step down
        const Member *highestPriority = theReplSet->getMostElectable();
        const Member *primary = rs->box.getPrimary();
        
        if (primary && highestPriority &&
            highestPriority->config().priority > primary->config().priority &&
            // if we're stepping down to allow another member to become primary, we
            // better have another member (otherOp), and it should be up-to-date
            otherOp != 0 && highestPriority->hbinfo().opTime.getSecs() >= otherOp - 10) {
            log() << "stepping down " << primary->fullName() << " (priority " <<
                primary->config().priority << "), " << highestPriority->fullName() <<
                " is priority " << highestPriority->config().priority << " and " <<
                (otherOp - highestPriority->hbinfo().opTime.getSecs()) << " seconds behind" << endl;

            if (primary->h().isSelf()) {
                // replSetStepDown tries to acquire the same lock
                // msgCheckNewState takes, so we can't call replSetStepDown on
                // ourselves.
                rs->relinquish();
            }
            else {
                BSONObj cmd = BSON( "replSetStepDown" << 1 );
                ScopedConn conn(primary->fullName());
                BSONObj result;

                try {
                    if (!conn.runCommand("admin", cmd, result, 0)) {
                        log() << "stepping down " << primary->fullName()
                              << " failed: " << result << endl;
                    }
                }
                catch (DBException &e) {
                    log() << "stepping down " << primary->fullName() << " threw exception: "
                          << e.toString() << endl;
                }
            }
        }
    }

    void Manager::checkAuth() {
        int down = 0, authIssue = 0, total = 0;

        for( Member *m = rs->head(); m; m=m->next() ) {
            total++;

            // all authIssue servers will also be not up
            if (!m->hbinfo().up()) {
                down++;
                if (m->hbinfo().authIssue) {
                    authIssue++;
                }
            }
        }

        // if all nodes are down or failed auth AND at least one failed
        // auth, go into recovering.  If all nodes are down, stay a
        // secondary.
        if (authIssue > 0 && down == total) {
            log() << "replset error could not reach/authenticate against any members" << endl;

            if (rs->box.getPrimary() == rs->_self) {
                log() << "auth problems, relinquishing primary" << rsLog;
                rs->relinquish();
            }

            rs->blockSync(true);
        }
        else {
            rs->blockSync(false);
        }
    }

    /** called as the health threads get new results */
    void Manager::msgCheckNewState() {
        {
            theReplSet->assertValid();
            rs->assertValid();

            RSBase::lock lk(rs);

            if( busyWithElectSelf ) return;
            
            checkElectableSet();
            checkAuth();

            const Member *p = rs->box.getPrimary();
            if( p && p != rs->_self ) {
                if( !p->hbinfo().up() ||
                        !p->hbinfo().hbstate.primary() ) {
                    p = 0;
                    rs->box.setOtherPrimary(0);
                }
            }

            const Member *p2;
            {
                bool two;
                p2 = findOtherPrimary(two);
                if( two ) {
                    /* two other nodes think they are primary (asynchronously polled) -- wait for things to settle down. */
                    log() << "replSet info two primaries (transiently)" << rsLog;
                    return;
                }
            }

            if( p2 ) {
                noteARemoteIsPrimary(p2);
                return;
            }

            /* didn't find anyone who wants to be primary */

            if( p ) {
                /* we are already primary */

                if( p != rs->_self ) {
                    rs->sethbmsg("error p != rs->self in checkNewState");
                    log() << "replSet " << p->fullName() << rsLog;
                    log() << "replSet " << rs->_self->fullName() << rsLog;
                    return;
                }

                if( rs->elect.shouldRelinquish() ) {
                    log() << "can't see a majority of the set, relinquishing primary" << rsLog;
                    rs->relinquish();
                }

                return;
            }

            if( !rs->iAmPotentiallyHot() ) { // if not we never try to be primary
                OCCASIONALLY log() << "replSet I don't see a primary and I can't elect myself" << endl;
                return;
            }

            /* no one seems to be primary.  shall we try to elect ourself? */
            if( !rs->elect.aMajoritySeemsToBeUp() ) {
                static time_t last;
                static int n;
                int ll = 0;
                if( ++n > 5 ) ll++;
                if( last + 60 > time(0 ) ) ll++;
                LOG(ll) << "replSet can't see a majority, will not try to elect self" << rsLog;
                last = time(0);
                return;
            }

            if( !rs->iAmElectable() ) {
                return;
            }

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
