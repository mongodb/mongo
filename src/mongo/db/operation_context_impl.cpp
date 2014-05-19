/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/operation_context_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"


namespace mongo {

    OperationContextImpl::OperationContextImpl() {
        _recovery.reset(new DurRecoveryUnit());
    }

    RecoveryUnit* OperationContextImpl::recoveryUnit() const {
        return _recovery.get();
    }

    LockState* OperationContextImpl::lockState() const {
        // TODO: This will eventually become member of OperationContextImpl
        return &cc().lockState();
    }

    ProgressMeter* OperationContextImpl::setMessage(const char* msg,
                                              const std::string& name,
                                              unsigned long long progressMeterTotal,
                                              int secondsBetween) {
        return &cc().curop()->setMessage( msg, name, progressMeterTotal, secondsBetween );
    }

    void OperationContextImpl::checkForInterrupt(bool heedMutex) const {
        killCurrentOp.checkForInterrupt(heedMutex);
    }

    Status OperationContextImpl::checkForInterruptNoAssert() const {
        const char* killed = killCurrentOp.checkForInterruptNoAssert();
        if ( !killed || !killed[0] )
            return Status::OK();

        return Status( ErrorCodes::Interrupted, killed );
    }

    bool OperationContextImpl::isPrimaryFor( const StringData& ns ) {
        string s = ns.toString(); // TODO: fix copy
        return isMasterNs( s.c_str() );
    }

    OperationContext* OperationContextImpl::factory() {
        return new OperationContextImpl();
    }

}  // namespace mongo
