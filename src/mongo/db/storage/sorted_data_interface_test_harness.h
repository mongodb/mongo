// sorted_data_interface_test_harness.h

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

#include <initializer_list>
#include <memory>

#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/unowned_ptr.h"

namespace mongo {

const BSONObj key0 = BSON("" << 0);
const BSONObj key1 = BSON("" << 1);
const BSONObj key2 = BSON("" << 2);
const BSONObj key3 = BSON("" << 3);
const BSONObj key4 = BSON("" << 4);
const BSONObj key5 = BSON("" << 5);
const BSONObj key6 = BSON("" << 6);

const BSONObj compoundKey1a = BSON("" << 1 << ""
                                      << "a");
const BSONObj compoundKey1b = BSON("" << 1 << ""
                                      << "b");
const BSONObj compoundKey1c = BSON("" << 1 << ""
                                      << "c");
const BSONObj compoundKey1d = BSON("" << 1 << ""
                                      << "d");
const BSONObj compoundKey2a = BSON("" << 2 << ""
                                      << "a");
const BSONObj compoundKey2b = BSON("" << 2 << ""
                                      << "b");
const BSONObj compoundKey2c = BSON("" << 2 << ""
                                      << "c");
const BSONObj compoundKey3a = BSON("" << 3 << ""
                                      << "a");
const BSONObj compoundKey3b = BSON("" << 3 << ""
                                      << "b");
const BSONObj compoundKey3c = BSON("" << 3 << ""
                                      << "c");

const RecordId loc1(0, 42);
const RecordId loc2(0, 44);
const RecordId loc3(0, 46);
const RecordId loc4(0, 48);
const RecordId loc5(0, 50);
const RecordId loc6(0, 52);
const RecordId loc7(0, 54);
const RecordId loc8(0, 56);

class RecoveryUnit;

class HarnessHelper {
public:
    HarnessHelper() : _serviceContext(), _client(_serviceContext.makeClient("hh")) {}
    virtual ~HarnessHelper() {}

    virtual std::unique_ptr<SortedDataInterface> newSortedDataInterface(bool unique) = 0;
    virtual std::unique_ptr<RecoveryUnit> newRecoveryUnit() = 0;

    ServiceContext::UniqueOperationContext newOperationContext(Client* client) {
        auto opCtx = client->makeOperationContext();
        opCtx->setRecoveryUnit(newRecoveryUnit().release(), OperationContext::kNotInUnitOfWork);
        return opCtx;
    }

    ServiceContext::UniqueOperationContext newOperationContext() {
        return newOperationContext(_client.get());
    }

    /**
     * Creates a new SDI with some initial data.
     *
     * For clarity to readers, toInsert must be sorted.
     */
    std::unique_ptr<SortedDataInterface> newSortedDataInterface(
        bool unique, std::initializer_list<IndexKeyEntry> toInsert);

    Client* client() {
        return _client.get();
    }

    ServiceContext* serviceContext() {
        return &_serviceContext;
    }

private:
    ServiceContextNoop _serviceContext;
    ServiceContext::UniqueClient _client;
};

/**
 * Inserts all entries in toInsert into index.
 * ASSERT_OKs the inserts.
 * Always uses dupsAllowed=true.
 *
 * Should be used for declaring and changing conditions, not for testing inserts.
 */
void insertToIndex(unowned_ptr<OperationContext> txn,
                   unowned_ptr<SortedDataInterface> index,
                   std::initializer_list<IndexKeyEntry> toInsert);

inline void insertToIndex(unowned_ptr<HarnessHelper> harness,
                          unowned_ptr<SortedDataInterface> index,
                          std::initializer_list<IndexKeyEntry> toInsert) {
    auto client = harness->serviceContext()->makeClient("insertToIndex");
    insertToIndex(harness->newOperationContext(client.get()), index, toInsert);
}

/**
 * Removes all entries in toRemove from index.
 * Always uses dupsAllowed=true.
 *
 * Should be used for declaring and changing conditions, not for testing removes.
 */
void removeFromIndex(unowned_ptr<OperationContext> txn,
                     unowned_ptr<SortedDataInterface> index,
                     std::initializer_list<IndexKeyEntry> toRemove);

inline void removeFromIndex(unowned_ptr<HarnessHelper> harness,
                            unowned_ptr<SortedDataInterface> index,
                            std::initializer_list<IndexKeyEntry> toRemove) {
    auto client = harness->serviceContext()->makeClient("removeFromIndex");
    removeFromIndex(harness->newOperationContext(client.get()), index, toRemove);
}

std::unique_ptr<HarnessHelper> newHarnessHelper();
}
