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
        return killImpl(i);
    }

    void KillCurrentOp::blockingKill(AtomicUInt opId) {
        bool killed = false;
        LOG(3) << "KillCurrentOp: starting blockingkill" << endl;
        boost::unique_lock<boost::mutex> lck(_mtx);
        bool foundId = killImpl(opId, &killed);
        if (!foundId) return; // don't wait if not found

        // block until the killed operation stops
        LOG(3) << "KillCurrentOp: waiting for confirmation of kill" << endl;
        while (killed == false) {
            _condvar.wait(lck);
        }
        LOG(3) << "KillCurrentOp: kill syncing complete" << endl;
    }

    bool KillCurrentOp::killImpl(AtomicUInt i, bool* pNotifyFlag /* = NULL */) {
        bool found = false;
        {
            scoped_lock l( Client::clientsMutex );
            for( set< Client* >::const_iterator j = Client::clients.begin(); 
                 !found && j != Client::clients.end(); 
                 ++j ) {
                for( CurOp *k = ( *j )->curop(); !found && k; k = k->parent() ) {
                    if ( k->opNum() == i ) {
                        k->kill(pNotifyFlag);
                        for( CurOp *l = ( *j )->curop(); l != k; l = l->parent() ) {
                            l->kill();
                        }
                        found = true;
                    }
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

    void KillCurrentOp::checkForInterrupt( bool heedMutex ) {
        Client& c = cc();
        if ( heedMutex && Lock::somethingWriteLocked() && c.hasWrittenThisPass() )
            return;
        if( _globalKill )
            uasserted(11600,"interrupted at shutdown");
        if( c.curop()->killPending() ) {
            notifyAllWaiters();
            uasserted(11601,"operation was interrupted");
        }
    }
    
    const char * KillCurrentOp::checkForInterruptNoAssert() {
        Client& c = cc();
        if( _globalKill )
            return "interrupted at shutdown";
        if( c.curop()->killPending() )
            return "interrupted";
        return "";
    }
}
