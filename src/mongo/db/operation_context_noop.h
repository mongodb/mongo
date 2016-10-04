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
#pragma once

#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

class Client;

class OperationContextNoop : public OperationContext {
public:
    /**
     * These constructors are for use in legacy tests that do not need operation contexts that are
     * properly connected to clients.
     */
    OperationContextNoop() : OperationContextNoop(nullptr, 0) {}
    OperationContextNoop(RecoveryUnit* ru) : OperationContextNoop(nullptr, 0) {
        setRecoveryUnit(ru, kNotInUnitOfWork);
    }


    /**
     * This constructor is for use by ServiceContexts, and should not be called directly.
     */
    OperationContextNoop(Client* client, unsigned int opId) : OperationContext(client, opId) {
        setRecoveryUnit(new RecoveryUnitNoop(), kNotInUnitOfWork);
        setLockState(stdx::make_unique<LockerNoop>());
    }

    virtual ~OperationContextNoop() = default;

    virtual ProgressMeter* setMessage_inlock(const char* msg,
                                             const std::string& name,
                                             unsigned long long progressMeterTotal,
                                             int secondsBetween) override {
        return &_pm;
    }

private:
    ProgressMeter _pm;
};

}  // namespace mongo
