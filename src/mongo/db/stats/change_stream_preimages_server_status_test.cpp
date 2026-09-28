// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_mock.h"
#include "mongo/db/storage/devnull/ephemeral_catalog_record_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

// The error code thrown by WiredTiger when the configured session_max is exceeded.
constexpr int kSessionMaxExceeded = 10828100;

/**
 * A record store that can be configured to throw the kind of transient storage-engine exception
 * (e.g. "Exceeded configured WiredTiger session_max") that the 'changeStreamPreImages'
 * serverStatus section must tolerate.
 */
class TestRecordStore final : public EphemeralForTestRecordStore {
public:
    TestRecordStore(bool shouldThrow, std::shared_ptr<void>* dataInOut)
        : EphemeralForTestRecordStore(
              UUID::gen(), "changeStreamPreImagesTestRecordStore", dataInOut),
          _shouldThrow(shouldThrow) {}

    int64_t storageSize(RecoveryUnit&, BSONObjBuilder* = nullptr, int = 0) const override {
        if (_shouldThrow) {
            uasserted(kSessionMaxExceeded,
                      "Exceeded configured WiredTiger session_max=16000 while opening a WiredTiger "
                      "session");
        }
        return kStorageSize;
    }

    int64_t freeStorageSize(RecoveryUnit&) const override {
        if (_shouldThrow) {
            uasserted(kSessionMaxExceeded,
                      "Exceeded configured WiredTiger session_max=16000 while opening a WiredTiger "
                      "session");
        }
        return kFreeStorageSize;
    }

    static constexpr int64_t kStorageSize = 1234;
    static constexpr int64_t kFreeStorageSize = 567;

private:
    bool _shouldThrow;
};

/**
 * A pre-images collection mock whose collection-level size attributes are directly controllable,
 * and which exposes the given record store.
 */
class TestPreImagesCollection final : public CollectionMock {
public:
    TestPreImagesCollection(RecordStore* recordStore,
                            long long numRecords,
                            long long dataSize,
                            int averageObjectSize)
        : CollectionMock(NamespaceString::kChangeStreamPreImagesNamespace),
          _recordStore(recordStore),
          _numRecords(numRecords),
          _dataSize(dataSize),
          _averageObjectSize(averageObjectSize) {}

    RecordStore* getRecordStore() const override {
        return _recordStore;
    }

    long long numRecords(OperationContext*) const override {
        return _numRecords;
    }

    long long dataSize(OperationContext*) const override {
        return _dataSize;
    }

    int averageObjectSize(OperationContext*) const override {
        return _averageObjectSize;
    }

private:
    RecordStore* _recordStore;
    long long _numRecords;
    long long _dataSize;
    int _averageObjectSize;
};

template <typename T>
T& mutableRef(const T& value) {
    return const_cast<T&>(value);
}

class ChangeStreamPreImagesServerStatusTestBase : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ChangeStreamOptionsManager::create(getServiceContext());
    }

    ServerStatusSection* section() {
        auto registry = ServerStatusSectionRegistry::instance();
        auto it = std::find_if(registry->begin(), registry->end(), [](const auto& entry) {
            return entry.second->getSectionName() == "changeStreamPreImages";
        });
        return it == registry->end() ? nullptr : it->second.get();
    }

    BSONObj generateSection() {
        auto* preImagesSection = section();
        ASSERT(preImagesSection);
        return preImagesSection->generateSection(operationContext(), BSONElement{});
    }
};

/**
 * Tests the fields of the section whose data is sourced from the pre-images collection and
 * ChangeStreamOptionsManager. The record store is mocked so its failure modes are controllable.
 */
class ChangeStreamPreImagesServerStatusMockTest : public ChangeStreamPreImagesServerStatusTestBase {
protected:
    void registerPreImagesCollection(bool shouldThrow,
                                     long long numRecords = 0,
                                     long long dataSize = 0,
                                     int averageObjectSize = 0) {
        _recordStoreData.reset();
        _recordStore = std::make_unique<TestRecordStore>(shouldThrow, &_recordStoreData);

        auto collection = std::make_shared<TestPreImagesCollection>(
            _recordStore.get(), numRecords, dataSize, averageObjectSize);

        auto opCtx = operationContext();
        Lock::GlobalWrite lk(opCtx);
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.registerCollection(opCtx, std::move(collection), boost::none);
        });
    }

    void setExpireAfterSeconds(int64_t seconds) {
        auto opCtx = operationContext();
        auto& optionsManager = ChangeStreamOptionsManager::get(opCtx);
        auto options = optionsManager.getOptions(opCtx);
        auto preAndPostImages = options.getPreAndPostImages();
        preAndPostImages.setExpireAfterSeconds(seconds);
        options.setPreAndPostImages(preAndPostImages);
        invariantStatusOK(optionsManager.setOptions(opCtx, options));
    }

    // Declared before '_recordStore' so the backing data outlives the record store.
    std::shared_ptr<void> _recordStoreData;
    std::unique_ptr<TestRecordStore> _recordStore;
};

TEST_F(ChangeStreamPreImagesServerStatusMockTest, ReportsZeroStorageSizesWhenRecordStoreThrows) {
    registerPreImagesCollection(/*shouldThrow=*/true, /*numRecords=*/1, /*dataSize=*/10);

    auto result = generateSection();

    ASSERT_EQ(result["storageSize"].numberLong(), 0LL);
    ASSERT_EQ(result["freeStorageSize"].numberLong(), 0LL);
    // Other metrics must still be reported even when the record store queries fail.
    ASSERT(result.hasField("numDocs"));
    ASSERT(result.hasField("purgingJob"));
    ASSERT(result.hasField("markerCreation"));
}

TEST_F(ChangeStreamPreImagesServerStatusMockTest, ReportsStorageSizesWhenRecordStoreSucceeds) {
    registerPreImagesCollection(/*shouldThrow=*/false);

    auto result = generateSection();

    ASSERT_EQ(result["storageSize"].numberLong(), TestRecordStore::kStorageSize);
    ASSERT_EQ(result["freeStorageSize"].numberLong(), TestRecordStore::kFreeStorageSize);
}

TEST_F(ChangeStreamPreImagesServerStatusMockTest, ReportsNumDocsTotalBytesAndAvgDocSize) {
    registerPreImagesCollection(
        /*shouldThrow=*/false, /*numRecords=*/4, /*dataSize=*/100, /*averageObjectSize=*/25);

    auto result = generateSection();

    ASSERT_EQ(result["numDocs"].numberLong(), 4LL);
    ASSERT_EQ(result["totalBytes"].numberLong(), 100LL);
    ASSERT_EQ(result["avgDocSize"].numberInt(), 25);
}

TEST_F(ChangeStreamPreImagesServerStatusMockTest, OmitsAvgDocSizeForEmptyCollection) {
    registerPreImagesCollection(
        /*shouldThrow=*/false, /*numRecords=*/0, /*dataSize=*/40, /*averageObjectSize=*/7);

    auto result = generateSection();

    ASSERT_EQ(result["numDocs"].numberLong(), 0LL);
    ASSERT_EQ(result["totalBytes"].numberLong(), 40LL);
    ASSERT_FALSE(result.hasField("avgDocSize"));
}

TEST_F(ChangeStreamPreImagesServerStatusMockTest, OmitsExpireAfterSecondsByDefault) {
    registerPreImagesCollection(/*shouldThrow=*/false, /*numRecords=*/1, /*dataSize=*/10);

    auto result = generateSection();

    ASSERT_FALSE(result.hasField("expireAfterSeconds"));
}

TEST_F(ChangeStreamPreImagesServerStatusMockTest, ReportsExpireAfterSecondsWhenConfigured) {
    setExpireAfterSeconds(3600);
    registerPreImagesCollection(/*shouldThrow=*/false, /*numRecords=*/1, /*dataSize=*/10);

    auto result = generateSection();

    ASSERT_EQ(result["expireAfterSeconds"].numberLong(), 3600LL);
}

TEST_F(ChangeStreamPreImagesServerStatusMockTest, ReportsPurgingJobAndMarkerCreationStats) {
    registerPreImagesCollection(/*shouldThrow=*/false, /*numRecords=*/1, /*dataSize=*/10);

    auto opCtx = operationContext();
    auto& manager = ChangeStreamPreImagesCollectionManager::get(opCtx);

    auto& purgingJobStats = mutableRef(manager.getPurgingJobStats());
    purgingJobStats.totalPass.store(4);
    purgingJobStats.docsDeleted.store(40);
    purgingJobStats.bytesDeleted.store(400);
    purgingJobStats.scannedCollections.store(2);
    purgingJobStats.scannedInternalCollections.store(3);
    purgingJobStats.timeElapsedMillis.store(500);

    auto& markerCreationStats = mutableRef(manager.getMarkerCreationStats());
    markerCreationStats.totalPass.store(9);
    markerCreationStats.scannedInternalCollections.store(11);
    markerCreationStats.timeElapsedMillis.store(900);

    auto result = generateSection();

    ASSERT_BSONOBJ_EQ(result["purgingJob"].Obj(), manager.getPurgingJobStats().toBSON());
    ASSERT_BSONOBJ_EQ(result["markerCreation"].Obj(), manager.getMarkerCreationStats().toBSON());
    ASSERT_EQ(result["purgingJob"]["totalPass"].numberLong(), 4LL);
    ASSERT_EQ(result["markerCreation"]["totalPass"].numberLong(), 9LL);
}

/**
 * Tests the fields of the section sourced from the manager using a real pre-images collection, so
 * 'docsInserted' reflects genuine insertions.
 */
class ChangeStreamPreImagesServerStatusRealCollectionTest
    : public ChangeStreamPreImagesServerStatusTestBase {
protected:
    void setUp() override {
        ChangeStreamPreImagesServerStatusTestBase::setUp();

        auto opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        ChangeStreamPreImagesCollectionManager::get(operationContext())
            .createPreImagesCollection(operationContext());
    }

    void insertPreImage(const Timestamp& timestamp) {
        auto opCtx = operationContext();
        auto preImage = change_stream_pre_image_test_helper::makePreImage(
            UUID::gen(), timestamp, Date_t::now());
        WriteUnitOfWork wuow(opCtx);
        ChangeStreamPreImagesCollectionManager::get(opCtx).insertPreImage(opCtx, preImage);
        wuow.commit();
    }
};

TEST_F(ChangeStreamPreImagesServerStatusRealCollectionTest, ReportsDocsInserted) {
    insertPreImage(Timestamp(1, 1));
    insertPreImage(Timestamp(1, 2));
    insertPreImage(Timestamp(1, 3));

    auto result = generateSection();

    ASSERT_EQ(result["docsInserted"].numberLong(), 3LL);
    ASSERT(result.hasField("numDocs"));
    ASSERT(result.hasField("totalBytes"));
    ASSERT_GTE(result["totalBytes"].numberLong(), 0LL);
    ASSERT(result.hasField("storageSize"));
    ASSERT(result.hasField("freeStorageSize"));
}

}  // namespace
}  // namespace mongo
