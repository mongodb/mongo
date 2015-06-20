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


#include "mongo/db/operation_context_noop.h"

namespace mongo {

class Locker;

namespace repl {

/**
 * Mock implementation of OperationContext that can be used with real instances of LockManager.
 * Note this is not thread safe and the setter methods should only be called in the context
 * where access to this object is guaranteed to be serialized.
 */
class OperationContextReplMock : public OperationContextNoop {
public:
    OperationContextReplMock();
    explicit OperationContextReplMock(unsigned int opNum);
    OperationContextReplMock(Client* client, unsigned int opNum);
    virtual ~OperationContextReplMock();

    virtual void checkForInterrupt() override;

    virtual Status checkForInterruptNoAssert() override;

    void setCheckForInterruptStatus(Status status);

    virtual uint64_t getRemainingMaxTimeMicros() const override;

    void setRemainingMaxTimeMicros(uint64_t micros);

    void setReplicatedWrites(bool writesAreReplicated = true) override;

    bool writesAreReplicated() const override;

private:
    Status _checkForInterruptStatus;
    uint64_t _maxTimeMicrosRemaining;
    bool _writesAreReplicated;
};

}  // namespace repl
}  // namespace mongo
