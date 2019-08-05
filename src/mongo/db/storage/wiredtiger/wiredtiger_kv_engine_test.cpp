
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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_engine_test_harness.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

#include "mongo/base/init.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class WiredTigerKVHarnessHelper : public KVHarnessHelper {
public:
    WiredTigerKVHarnessHelper(bool forRepair = false)
        : _dbpath("wt-kv-harness"), _forRepair(forRepair) {
        if (!hasGlobalServiceContext())
            setGlobalServiceContext(ServiceContext::make());
        _engine.reset(makeEngine());
        repl::ReplicationCoordinator::set(
            getGlobalServiceContext(),
            std::unique_ptr<repl::ReplicationCoordinator>(new repl::ReplicationCoordinatorMock(
                getGlobalServiceContext(), repl::ReplSettings())));
    }

    virtual ~WiredTigerKVHarnessHelper() {
        _engine.reset(nullptr);
        // Cannot cleanup the global service context here, the test still have clients remaining.
    }

    virtual KVEngine* restartEngine() override {
        _engine.reset(nullptr);
        _engine.reset(makeEngine());
        return _engine.get();
    }

    virtual KVEngine* getEngine() override {
        return _engine.get();
    }

    virtual WiredTigerKVEngine* getWiredTigerKVEngine() {
        return _engine.get();
    }

private:
    WiredTigerKVEngine* makeEngine() {
        return new WiredTigerKVEngine(kWiredTigerEngineName,
                                      _dbpath.path(),
                                      _cs.get(),
                                      "",
                                      1,
                                      0,
                                      false,
                                      false,
                                      _forRepair,
                                      false);
    }

    const std::unique_ptr<ClockSource> _cs = stdx::make_unique<ClockSourceMock>();
    unittest::TempDir _dbpath;
    std::unique_ptr<WiredTigerKVEngine> _engine;
    bool _forRepair;
};

class WiredTigerKVEngineTest : public unittest::Test {
public:
    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
        Client::initThread(getThreadName());

        _helper = makeHelper();
        _engine = _helper->getWiredTigerKVEngine();
    }

    void tearDown() override {
        _helper.reset(nullptr);
        Client::destroy();
        setGlobalServiceContext({});
    }

    std::unique_ptr<OperationContext> makeOperationContext() {
        return std::make_unique<OperationContextNoop>(_engine->newRecoveryUnit());
    }

protected:
    virtual std::unique_ptr<WiredTigerKVHarnessHelper> makeHelper() {
        return std::make_unique<WiredTigerKVHarnessHelper>();
    }

    std::unique_ptr<WiredTigerKVHarnessHelper> _helper;
    WiredTigerKVEngine* _engine;
};

class WiredTigerKVEngineRepairTest : public WiredTigerKVEngineTest {
    virtual std::unique_ptr<WiredTigerKVHarnessHelper> makeHelper() override {
        return std::make_unique<WiredTigerKVHarnessHelper>(true /* repair */);
    }
};

TEST_F(WiredTigerKVEngineRepairTest, OrphanedDataFilesCanBeRecovered) {
    auto opCtxPtr = makeOperationContext();

    std::string ns = "a.b";
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions options;

    std::unique_ptr<RecordStore> rs;
    ASSERT_OK(_engine->createRecordStore(opCtxPtr.get(), ns, ident, options));
    rs = _engine->getRecordStore(opCtxPtr.get(), ns, ident, options);
    ASSERT(rs);

    RecordId loc;
    {
        WriteUnitOfWork uow(opCtxPtr.get());
        StatusWith<RecordId> res = rs->insertRecord(
            opCtxPtr.get(), record.c_str(), record.length() + 1, Timestamp(), false);
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    const boost::optional<boost::filesystem::path> dataFilePath =
        _engine->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);

    ASSERT(boost::filesystem::exists(*dataFilePath));

    const boost::filesystem::path tmpFile{dataFilePath->string() + ".tmp"};
    ASSERT(!boost::filesystem::exists(tmpFile));

#ifdef _WIN32
    auto status = _engine->recoverOrphanedIdent(opCtxPtr.get(), ns, ident, options);
    ASSERT_EQ(ErrorCodes::CommandNotSupported, status.code());
#else
    // Move the data file out of the way so the ident can be dropped. This not permitted on Windows
    // because the file cannot be moved while it is open. The implementation for orphan recovery is
    // also not implemented on Windows for this reason.
    boost::system::error_code err;
    boost::filesystem::rename(*dataFilePath, tmpFile, err);
    ASSERT(!err) << err.message();

    ASSERT_OK(_engine->dropIdent(opCtxPtr.get(), ident));

    // The data file is moved back in place so that it becomes an "orphan" of the storage
    // engine and the restoration process can be tested.
    boost::filesystem::rename(tmpFile, *dataFilePath, err);
    ASSERT(!err) << err.message();

    auto status = _engine->recoverOrphanedIdent(opCtxPtr.get(), ns, ident, options);
    ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code());
#endif
}

TEST_F(WiredTigerKVEngineRepairTest, UnrecoverableOrphanedDataFilesAreRebuilt) {
    auto opCtxPtr = makeOperationContext();

    std::string ns = "a.b";
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions options;

    std::unique_ptr<RecordStore> rs;
    ASSERT_OK(_engine->createRecordStore(opCtxPtr.get(), ns, ident, options));
    rs = _engine->getRecordStore(opCtxPtr.get(), ns, ident, options);
    ASSERT(rs);

    RecordId loc;
    {
        WriteUnitOfWork uow(opCtxPtr.get());
        StatusWith<RecordId> res = rs->insertRecord(
            opCtxPtr.get(), record.c_str(), record.length() + 1, Timestamp(), false);
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    const boost::optional<boost::filesystem::path> dataFilePath =
        _engine->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);

    ASSERT(boost::filesystem::exists(*dataFilePath));

    ASSERT_OK(_engine->dropIdent(opCtxPtr.get(), ident));

#ifdef _WIN32
    auto status = _engine->recoverOrphanedIdent(opCtxPtr.get(), ns, ident, options);
    ASSERT_EQ(ErrorCodes::CommandNotSupported, status.code());
#else
    // The ident may not get immediately dropped, so ensure it is completely gone.
    boost::system::error_code err;
    boost::filesystem::remove(*dataFilePath, err);
    ASSERT(!err) << err.message();

    // Create an empty data file. The subsequent call to recreate the collection will fail because
    // it is unsalvageable.
    boost::filesystem::ofstream fileStream(*dataFilePath);
    fileStream << "";
    fileStream.close();

    ASSERT(boost::filesystem::exists(*dataFilePath));

    // This should recreate an empty data file successfully and move the old one to a name that ends
    // in ".corrupt".
    auto status = _engine->recoverOrphanedIdent(opCtxPtr.get(), ns, ident, options);
    ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code()) << status.reason();

    boost::filesystem::path corruptFile = (dataFilePath->string() + ".corrupt");
    ASSERT(boost::filesystem::exists(corruptFile));

    rs = _engine->getRecordStore(opCtxPtr.get(), ns, ident, options);
    RecordData data;
    ASSERT_FALSE(rs->findRecord(opCtxPtr.get(), loc, &data));
#endif
}

std::unique_ptr<KVHarnessHelper> makeHelper() {
    return stdx::make_unique<WiredTigerKVHarnessHelper>();
}

MONGO_INITIALIZER(RegisterKVHarnessFactory)(InitializerContext*) {
    KVHarnessHelper::registerFactory(makeHelper);
    return Status::OK();
}

}  // namespace
}  // namespace mongo
