/**
*    Copyright (C) 2009 10gen Inc.
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

#include "mongo/db/kill_current_op.h"

#include <set>

#include "mongo/bson/util/atomic_int.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    void KillCurrentOp::interruptJs( AtomicUInt *op ) {
        if ( !globalScriptEngine )
            return;
        if ( !op ) {
            globalScriptEngine->interruptAll();
        }
        else {
            globalScriptEngine->interrupt( *op );
        }
    }

    void KillCurrentOp::killAll() {
        _globalKill = true;
        interruptJs( 0 );
    }

    bool KillCurrentOp::kill(AtomicUInt i) {
        scoped_lock l( Client::clientsMutex );
        return _killImpl_inclientlock(i);
    }

    void KillCurrentOp::blockingKill(AtomicUInt opId) {
        bool killed = false;
        LOG(1) << "KillCurrentOp: starting blockingkill" << endl;

        boost::scoped_ptr<scoped_lock> clientLock( new scoped_lock( Client::clientsMutex ) );
        boost::unique_lock<boost::mutex> lck(_mtx);

        bool foundId = _killImpl_inclientlock(opId, &killed);
        if (!foundId) {
            // don't wait if not found
            return;
        }

        clientLock.reset( NULL ); // unlock client since we don't need it anymore

        // block until the killed operation stops
        LOG(1) << "KillCurrentOp: waiting for confirmation of kill" << endl;
        while (killed == false) {
            _condvar.wait(lck);
        }
        LOG(1) << "KillCurrentOp: kill syncing complete" << endl;
    }

    bool KillCurrentOp::_killImpl_inclientlock(AtomicUInt i, bool* pNotifyFlag /* = NULL */) {
        bool found = false;
        {
            for( set< Client* >::const_iterator j = Client::clients.begin();
                 !found && j != Client::clients.end();
                 ++j ) {

                for( CurOp *k = ( *j )->curop(); !found && k; k = k->parent() ) {
                    if ( k->opNum() != i )
                        continue;

                    k->kill(pNotifyFlag);
                    for( CurOp *l = ( *j )->curop(); l; l = l->parent() ) {
                        l->kill();
                    }

                    found = true;
                }
            }
        }
        if ( found ) {
            interruptJs( &i );
        }
        return found;
    }


    void KillCurrentOp::notifyAllWaiters() {
        boost::unique_lock<boost::mutex> lck(_mtx);
        if (!haveClient()) 
            return;
        cc().curop()->setKillWaiterFlags();
        _condvar.notify_all();
    }

    void KillCurrentOp::checkForInterrupt(bool heedMutex) {
        Client& c = cc();

        if (heedMutex && Lock::somethingWriteLocked() && c.hasWrittenThisPass()) {
            return;
        }

        if (_globalKill) {
            uasserted(11600, "interrupted at shutdown");
        }
        if (c.curop()->maxTimeHasExpired()) {
            c.curop()->kill();
            notifyAllWaiters();
            uasserted(ErrorCodes::ExceededTimeLimit, "operation exceeded time limit");
        }
        if (c.curop()->killPending()) {
            notifyAllWaiters();
            uasserted(11601, "operation was interrupted");
        }
    }
    
    const char * KillCurrentOp::checkForInterruptNoAssert() {
        Client& c = cc();

        if (_globalKill) {
            return "interrupted at shutdown";
        }
        if (c.curop()->maxTimeHasExpired()) {
            c.curop()->kill();
            return "exceeded time limit";
        }
        if (c.curop()->killPending()) {
            return "interrupted";
        }
        return "";
    }

    void KillCurrentOp::reset() {
        _globalKill = false;
    }
}
