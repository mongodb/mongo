/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/test_harness_helper.h"

#include <functional>
#include <initializer_list>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const BSONObj key0 = BSON("" << 0);
const BSONObj key1 = BSON("" << 1);
const BSONObj key2 = BSON("" << 2);
const BSONObj key3 = BSON("" << 3);
const BSONObj key4 = BSON("" << 4);
const BSONObj key5 = BSON("" << 5);
const BSONObj key6 = BSON("" << 6);
const BSONObj key7 = BSON("" << "\x00");

const BSONObj key8 = BSON("" << "\xff");

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

class SortedDataInterfaceHarnessHelper : public virtual HarnessHelper {
public:
    virtual std::unique_ptr<SortedDataInterface> newSortedDataInterface(OperationContext* opCtx,
                                                                        bool unique,
                                                                        bool partial,
                                                                        KeyFormat keyFormat) = 0;

    std::unique_ptr<SortedDataInterface> newSortedDataInterface(OperationContext* opCtx,
                                                                bool unique,
                                                                bool partial) {
        return newSortedDataInterface(opCtx, unique, partial, KeyFormat::Long);
    }

    virtual std::unique_ptr<SortedDataInterface> newIdIndexSortedDataInterface(
        OperationContext* opCtx) = 0;
    /**
     * Creates a new SDI with some initial data.
     *
     * For clarity to readers, toInsert must be sorted.
     */
    std::unique_ptr<SortedDataInterface> newSortedDataInterface(
        OperationContext* opCtx,
        bool unique,
        bool partial,
        std::initializer_list<IndexKeyEntry> toInsert);
};

void registerSortedDataInterfaceHarnessHelperFactory(
    std::function<std::unique_ptr<SortedDataInterfaceHarnessHelper>(int32_t cacheSizeMB)> factory);

std::unique_ptr<SortedDataInterfaceHarnessHelper> newSortedDataInterfaceHarnessHelper(
    int32_t cacheSizeMB = 64);

key_string::Value makeKeyString(SortedDataInterface* sorted,
                                BSONObj bsonKey,
                                const boost::optional<RecordId>& rid = boost::none);

key_string::Builder makeKeyStringForSeek(SortedDataInterface* sorted, BSONObj bsonKey);

key_string::Builder makeKeyStringForSeek(SortedDataInterface* sorted,
                                         BSONObj bsonKey,
                                         bool isForward,
                                         bool inclusive);

/**
 * Inserts all entries in toInsert into index.
 * ASSERT_OKs the inserts.
 *
 * Should be used for declaring and changing conditions, not for testing inserts.
 */
void insertToIndex(OperationContext* opCtx,
                   SortedDataInterface* index,
                   std::initializer_list<IndexKeyEntry> toInsert,
                   bool dupsAllowed = true);

/**
 * Removes all entries in toRemove from index.
 * Always uses dupsAllowed=true.
 *
 * Should be used for declaring and changing conditions, not for testing removes.
 */
void removeFromIndex(OperationContext* opCtx,
                     SortedDataInterface* index,
                     std::initializer_list<IndexKeyEntry> toRemove);

class SortedDataInterfaceTest : public unittest::Test {
public:
    void setUp() override {
        _harnessHelper = newSortedDataInterfaceHarnessHelper();
        _opCtx = _harnessHelper->newOperationContext();
    }

    void tearDown() override {
        _opCtx.reset();
        _harnessHelper.reset();
    }

    SortedDataInterfaceHarnessHelper* harnessHelper() {
        return _harnessHelper.get();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    RecoveryUnit& recoveryUnit() {
        return *shard_role_details::getRecoveryUnit(opCtx());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<SortedDataInterfaceHarnessHelper> _harnessHelper;
};

}  // namespace mongo
