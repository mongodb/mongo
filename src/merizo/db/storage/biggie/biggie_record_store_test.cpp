/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/base/init.h"
#include "merizo/db/storage/biggie/biggie_kv_engine.h"
#include "merizo/db/storage/biggie/biggie_record_store.h"
#include "merizo/db/storage/biggie/biggie_recovery_unit.h"
#include "merizo/db/storage/biggie/store.h"
#include "merizo/db/storage/record_store_test_harness.h"
#include "merizo/stdx/memory.h"
#include "merizo/unittest/unittest.h"

namespace merizo {
namespace biggie {
namespace {

class RecordStoreHarnessHelper final : public ::merizo::RecordStoreHarnessHelper {
    KVEngine _kvEngine{};
    VisibilityManager _visibilityManager;

public:
    RecordStoreHarnessHelper() {}

    virtual std::unique_ptr<merizo::RecordStore> newNonCappedRecordStore() {
        return newNonCappedRecordStore("a.b");
    }

    virtual std::unique_ptr<merizo::RecordStore> newNonCappedRecordStore(const std::string& ns) {
        return std::make_unique<RecordStore>(ns,
                                             "ident"_sd /* ident */,
                                             false /* isCapped */,
                                             -1 /* cappedMaxSize */,
                                             -1 /* cappedMaxDocs */,
                                             nullptr /* cappedCallback */,
                                             nullptr /* visibilityManager */);
    }

    virtual std::unique_ptr<merizo::RecordStore> newCappedRecordStore(int64_t cappedSizeBytes,
                                                                     int64_t cappedMaxDocs) {
        return newCappedRecordStore("a.b", cappedSizeBytes, cappedMaxDocs);
    }

    virtual std::unique_ptr<merizo::RecordStore> newCappedRecordStore(const std::string& ns,
                                                                     int64_t cappedSizeBytes,
                                                                     int64_t cappedMaxDocs) final {
        return std::make_unique<RecordStore>(ns,
                                             "ident"_sd,
                                             /*isCapped*/ true,
                                             cappedSizeBytes,
                                             cappedMaxDocs,
                                             /*cappedCallback*/ nullptr,
                                             &_visibilityManager);
    }

    std::unique_ptr<merizo::RecoveryUnit> newRecoveryUnit() final {
        return std::make_unique<RecoveryUnit>(&_kvEngine);
    }

    bool supportsDocLocking() final {
        return true;
    }
};

std::unique_ptr<merizo::HarnessHelper> makeHarnessHelper() {
    return std::make_unique<RecordStoreHarnessHelper>();
}

MONGO_INITIALIZER(RegisterHarnessFactory)(InitializerContext* const) {
    merizo::registerHarnessHelperFactory(makeHarnessHelper);
    return Status::OK();
}
}  // namespace
}  // namespace biggie
}  // namespace merizo
