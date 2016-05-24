// record_store_test_harness.h

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

#include <cstdint>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class RecordStore;
class RecoveryUnit;

class HarnessHelper {
public:
    HarnessHelper() : _serviceContext(), _client(_serviceContext.makeClient("hh")) {}
    virtual ~HarnessHelper() {}

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore() = 0;

    static const int64_t kDefaultCapedSizeBytes = 16 * 1024 * 1024;
    virtual std::unique_ptr<RecordStore> newCappedRecordStore(
        int64_t cappedSizeBytes = kDefaultCapedSizeBytes, int64_t cappedMaxDocs = -1) = 0;

    virtual ServiceContext::UniqueOperationContext newOperationContext(Client* client) {
        auto opCtx = client->makeOperationContext();
        opCtx->setRecoveryUnit(newRecoveryUnit(), OperationContext::kNotInUnitOfWork);
        return opCtx;
    }

    ServiceContext::UniqueOperationContext newOperationContext() {
        return newOperationContext(_client.get());
    }

    /**
     * Currently this requires that it is possible to have two independent open write operations
     * at the same time one the same thread (with separate Clients, OperationContexts, and
     * RecoveryUnits).
     */
    virtual bool supportsDocLocking() = 0;

    Client* client() {
        return _client.get();
    }
    ServiceContext* serviceContext() {
        return &_serviceContext;
    }

private:
    virtual RecoveryUnit* newRecoveryUnit() = 0;

    ServiceContextNoop _serviceContext;
    ServiceContext::UniqueClient _client;
};

std::unique_ptr<HarnessHelper> newHarnessHelper();
}
