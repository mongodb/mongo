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


#include "mongo/db/operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/curop.h"
#include "mongo/db/storage/recovery_unit_noop.h"

namespace mongo {

class OperationContextNoop : public OperationContext {
public:
    OperationContextNoop() : OperationContextNoop(new RecoveryUnitNoop()) {}

    OperationContextNoop(RecoveryUnit* ru) : OperationContextNoop(nullptr, 0, ru) {}

    OperationContextNoop(Client* client, unsigned int opId)
        : OperationContextNoop(client, opId, new RecoveryUnitNoop()) {}

    OperationContextNoop(Client* client, unsigned int opId, RecoveryUnit* ru)
        : OperationContextNoop(client, opId, new LockerNoop(), ru) {}

    OperationContextNoop(Client* client, unsigned int opId, Locker* locker)
        : OperationContextNoop(client, opId, locker, new RecoveryUnitNoop()) {}

    OperationContextNoop(Client* client, unsigned int opId, Locker* locker, RecoveryUnit* ru)
        : OperationContext(client, opId, locker), _recoveryUnit(ru) {
        _locker.reset(lockState());

        if (client) {
            stdx::lock_guard<Client> lk(*client);
            client->setOperationContext(this);
        }
    }

    virtual ~OperationContextNoop() {
        auto client = getClient();
        if (client) {
            stdx::lock_guard<Client> lk(*client);
            client->resetOperationContext();
        }
    }

    virtual RecoveryUnit* recoveryUnit() const override {
        return _recoveryUnit.get();
    }

    virtual RecoveryUnit* releaseRecoveryUnit() override {
        return _recoveryUnit.release();
    }

    virtual RecoveryUnitState setRecoveryUnit(RecoveryUnit* unit,
                                              RecoveryUnitState state) override {
        RecoveryUnitState oldState = _ruState;
        _recoveryUnit.reset(unit);
        _ruState = state;
        return oldState;
    }

    virtual ProgressMeter* setMessage_inlock(const char* msg,
                                             const std::string& name,
                                             unsigned long long progressMeterTotal,
                                             int secondsBetween) override {
        return &_pm;
    }

    virtual bool isPrimaryFor(StringData ns) override {
        return true;
    }

    virtual std::string getNS() const override {
        return std::string();
    };

    void setReplicatedWrites(bool writesAreReplicated = true) override {}

    bool writesAreReplicated() const override {
        return false;
    }

private:
    std::unique_ptr<RecoveryUnit> _recoveryUnit;
    std::unique_ptr<Locker> _locker;
    ProgressMeter _pm;
};

}  // namespace mongo
