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
#include "mongo/platform/random.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

    // Enabling the checkForInterruptFail fail point will start a game of random chance on the
    // connection specified in the fail point data, generating an interrupt with a given fixed
    // probability.  Example invocation:
    //
    // {configureFailPoint: "checkForInterruptFail",
    //  mode: "alwaysOn",
    //  data: {conn: 17, chance: .01, allowNested: true}}
    //
    // All three data fields must be specified.  In the above example, all interrupt points on
    // connection 17 will generate a kill on the current operation with probability p(.01),
    // including interrupt points of nested operations.  If "allowNested" is false, nested
    // operations are not targeted.  "chance" must be a double between 0 and 1, inclusive.
    MONGO_FP_DECLARE(checkForInterruptFail);

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

    bool KillCurrentOp::_killImpl_inclientlock(AtomicUInt i) {
        bool found = false;
        {
            for( set< Client* >::const_iterator j = Client::clients.begin();
                 !found && j != Client::clients.end();
                 ++j ) {

                for( CurOp *k = ( *j )->curop(); !found && k; k = k->parent() ) {
                    if ( k->opNum() != i )
                        continue;

                    k->kill();
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

namespace {

    // Global state for checkForInterrupt fail point.
    PseudoRandom checkForInterruptPRNG(static_cast<int64_t>(time(NULL)));

    // Helper function for checkForInterrupt fail point.  Decides whether the operation currently
    // being run by the given Client meet the (probabilistic) conditions for interruption as
    // specified in the fail point info.
    bool opShouldFail(const Client& c, const BSONObj& failPointInfo) {
        // Only target the client with the specified connection number.
        if (c.getConnectionId() != failPointInfo["conn"].safeNumberLong()) {
            return false;
        }

        // Only target nested operations if requested.
        if (!failPointInfo["allowNested"].trueValue() && c.curop()->parent() != NULL) {
            return false;
        }

        // Return true with (approx) probability p = "chance".  Recall: 0 <= chance <= 1.
        double next = static_cast<double>(std::abs(checkForInterruptPRNG.nextInt64()));
        double upperBound =
            std::numeric_limits<int64_t>::max() * failPointInfo["chance"].numberDouble();
        if (next > upperBound) {
            return false;
        }
        return true;
    }

} // namespace

    void KillCurrentOp::checkForInterrupt(bool heedMutex) {
        Client& c = cc();

        if (heedMutex && Lock::somethingWriteLocked() && c.hasWrittenSinceCheckpoint()) {
            return;
        }

        uassert(ErrorCodes::InterruptedAtShutdown, "interrupted at shutdown", !_globalKill);

        if (c.curop()->maxTimeHasExpired()) {
            c.curop()->kill();
            uasserted(ErrorCodes::ExceededTimeLimit, "operation exceeded time limit");
        }

        MONGO_FAIL_POINT_BLOCK(checkForInterruptFail, scopedFailPoint) {
            if (opShouldFail(c, scopedFailPoint.getData())) {
                log() << "set pending kill on " << (c.curop()->parent() ? "nested" : "top-level")
                      << " op " << c.curop()->opNum().get() << ", for checkForInterruptFail";
                c.curop()->kill();
            }
        }

        if (c.curop()->killPending()) {
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
        MONGO_FAIL_POINT_BLOCK(checkForInterruptFail, scopedFailPoint) {
            if (opShouldFail(c, scopedFailPoint.getData())) {
                log() << "set pending kill on " << (c.curop()->parent() ? "nested" : "top-level")
                      << " op " << c.curop()->opNum().get() << ", for checkForInterruptFail";
                c.curop()->kill();
            }
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
