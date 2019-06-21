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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/durable_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using NonRepairableFeature = DurableCatalogImpl::FeatureTracker::NonRepairableFeature;
using NonRepairableFeatureMask = DurableCatalogImpl::FeatureTracker::NonRepairableFeatureMask;
using RepairableFeature = DurableCatalogImpl::FeatureTracker::RepairableFeature;
using RepairableFeatureMask = DurableCatalogImpl::FeatureTracker::RepairableFeatureMask;

class DurableCatalogFeatureTrackerTest : public unittest::Test {
public:
    static const NonRepairableFeature kNonRepairableFeature1 =
        static_cast<NonRepairableFeature>(1 << 0);

    static const NonRepairableFeature kNonRepairableFeature2 =
        static_cast<NonRepairableFeature>(1 << 1);

    static const NonRepairableFeature kNonRepairableFeature3 =
        static_cast<NonRepairableFeature>(1 << 2);

    static const RepairableFeature kRepairableFeature1 = static_cast<RepairableFeature>(1 << 0);

    static const RepairableFeature kRepairableFeature2 = static_cast<RepairableFeature>(1 << 1);

    static const RepairableFeature kRepairableFeature3 = static_cast<RepairableFeature>(1 << 2);

    DurableCatalogFeatureTrackerTest() : _helper(KVHarnessHelper::create()) {}

    std::unique_ptr<OperationContext> newOperationContext() {
        return stdx::make_unique<OperationContextNoop>(_helper->getEngine()->newRecoveryUnit());
    }

    void setUp() final {
        auto opCtx = newOperationContext();
        {
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(_helper->getEngine()->createRecordStore(
                opCtx.get(), "catalog", "catalog", CollectionOptions()));
            _rs = _helper->getEngine()->getRecordStore(
                opCtx.get(), "catalog", "catalog", CollectionOptions());
            wuow.commit();
        }

        _catalog = std::make_unique<DurableCatalogImpl>(_rs.get(), false, false, nullptr);
        _catalog->init(opCtx.get());

        {
            WriteUnitOfWork wuow(opCtx.get());
            _featureTracker =
                DurableCatalogImpl::FeatureTracker::create(opCtx.get(), _catalog.get());
            wuow.commit();
        }
    }

    RecordStore* getRecordStore() const {
        return _rs.get();
    }

    DurableCatalogImpl::FeatureTracker* getFeatureTracker() const {
        return _featureTracker.get();
    }

private:
    std::unique_ptr<KVHarnessHelper> _helper;
    std::unique_ptr<RecordStore> _rs;
    std::unique_ptr<DurableCatalogImpl> _catalog;
    std::unique_ptr<DurableCatalogImpl::FeatureTracker> _featureTracker;
};

TEST_F(DurableCatalogFeatureTrackerTest, FeatureDocumentIsNotEagerlyCreated) {
    auto opCtx = newOperationContext();
    auto cursor = getRecordStore()->getCursor(opCtx.get());
    ASSERT_FALSE(static_cast<bool>(cursor->next()));
}

TEST_F(DurableCatalogFeatureTrackerTest, CanMarkNonRepairableFeatureAsInUse) {
    {
        auto opCtx = newOperationContext();
        ASSERT(
            !getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature1);
            wuow.commit();
        }
        ASSERT(
            getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));

        // Marking the same non-repairable feature as in-use again does nothing.
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature1);
            wuow.commit();
        }
        ASSERT(
            getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));
    }

    // The repairable feature bit in the same position is unaffected.
    {
        auto opCtx = newOperationContext();
        ASSERT(!getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));
    }

    // The non-repairable feature in a different position is unaffected.
    {
        auto opCtx = newOperationContext();
        ASSERT(
            !getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature2));
    }
}

TEST_F(DurableCatalogFeatureTrackerTest, CanMarkNonRepairableFeatureAsNotInUse) {
    {
        auto opCtx = newOperationContext();

        ASSERT(
            !getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature1);
            wuow.commit();
        }
        ASSERT(
            getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));

        ASSERT(!getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature1);
            wuow.commit();
        }
        ASSERT(getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));

        ASSERT(
            !getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature2));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature2);
            wuow.commit();
        }
        ASSERT(
            getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature2));
    }

    {
        auto opCtx = newOperationContext();
        ASSERT(
            getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsNotInUse(opCtx.get(),
                                                                    kNonRepairableFeature1);
            wuow.commit();
        }
        ASSERT(
            !getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));

        // Marking the same non-repairable feature as not in-use again does nothing.
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsNotInUse(opCtx.get(),
                                                                    kNonRepairableFeature1);
            wuow.commit();
        }
        ASSERT(
            !getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));
    }

    // The repairable feature bit in the same position is unaffected.
    {
        auto opCtx = newOperationContext();
        ASSERT(getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));
    }

    // The non-repairable feature in a different position is unaffected.
    {
        auto opCtx = newOperationContext();
        ASSERT(
            getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature2));
    }
}

TEST_F(DurableCatalogFeatureTrackerTest, CanMarkRepairableFeatureAsInUse) {
    {
        auto opCtx = newOperationContext();
        ASSERT(!getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature1);
            wuow.commit();
        }
        ASSERT(getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));

        // Marking the same repairable feature as in-use again does nothing.
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature1);
            wuow.commit();
        }
        ASSERT(getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));
    }

    // The non-repairable feature bit in the same position is unaffected.
    {
        auto opCtx = newOperationContext();
        ASSERT(
            !getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));
    }

    // The repairable feature in a different position is unaffected.
    {
        auto opCtx = newOperationContext();
        ASSERT(!getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature2));
    }
}

TEST_F(DurableCatalogFeatureTrackerTest, CanMarkRepairableFeatureAsNotInUse) {
    {
        auto opCtx = newOperationContext();

        ASSERT(!getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature1);
            wuow.commit();
        }
        ASSERT(getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));

        ASSERT(
            !getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature1);
            wuow.commit();
        }
        ASSERT(
            getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));

        ASSERT(!getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature2));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature2);
            wuow.commit();
        }
        ASSERT(getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature2));
    }

    {
        auto opCtx = newOperationContext();
        ASSERT(getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsNotInUse(opCtx.get(), kRepairableFeature1);
            wuow.commit();
        }
        ASSERT(!getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));

        // Marking the same repairable feature as not in-use again does nothing.
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsNotInUse(opCtx.get(), kRepairableFeature1);
            wuow.commit();
        }
        ASSERT(!getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature1));
    }

    // The non-repairable feature bit in the same position is unaffected.
    {
        auto opCtx = newOperationContext();
        ASSERT(
            getFeatureTracker()->isNonRepairableFeatureInUse(opCtx.get(), kNonRepairableFeature1));
    }

    // The repairable feature in a different position is unaffected.
    {
        auto opCtx = newOperationContext();
        ASSERT(getFeatureTracker()->isRepairableFeatureInUse(opCtx.get(), kRepairableFeature2));
    }
}

TEST_F(DurableCatalogFeatureTrackerTest, DataFileAreCompatibleWithRecognizedNonRepairableFeature) {
    getFeatureTracker()->setUsedNonRepairableFeaturesMaskForTestingOnly(0ULL);
    getFeatureTracker()->setUsedRepairableFeaturesMaskForTestingOnly(0ULL);

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
    }

    getFeatureTracker()->setUsedNonRepairableFeaturesMaskForTestingOnly(
        static_cast<NonRepairableFeatureMask>(kNonRepairableFeature1));

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature1);
            wuow.commit();
        }
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
    }
}

TEST_F(DurableCatalogFeatureTrackerTest,
       DataFilesAreIncompatibleWithAnUnrecognizedNonRepairableFeature) {
    getFeatureTracker()->setUsedNonRepairableFeaturesMaskForTestingOnly(0ULL);
    getFeatureTracker()->setUsedRepairableFeaturesMaskForTestingOnly(0ULL);

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
    }

    {
        auto opCtx = newOperationContext();
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature1);
            wuow.commit();
        }

        auto status = getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get());
        ASSERT_EQ(ErrorCodes::MustUpgrade, status.code());
        ASSERT_EQ(
            "The data files use features not recognized by this version of mongod; the NR feature"
            " bits in positions [ 0 ] aren't recognized by this version of mongod",
            status.reason());
    }
}

TEST_F(DurableCatalogFeatureTrackerTest,
       DataFilesAreIncompatibleWithMultipleUnrecognizedNonRepairableFeatures) {
    getFeatureTracker()->setUsedNonRepairableFeaturesMaskForTestingOnly(
        static_cast<NonRepairableFeatureMask>(kNonRepairableFeature1));
    getFeatureTracker()->setUsedRepairableFeaturesMaskForTestingOnly(0ULL);

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
    }

    {
        auto opCtx = newOperationContext();
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature2);
            wuow.commit();
        }

        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx.get(),
                                                                 kNonRepairableFeature3);
            wuow.commit();
        }

        auto status = getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get());
        ASSERT_EQ(ErrorCodes::MustUpgrade, status.code());
        ASSERT_EQ(
            "The data files use features not recognized by this version of mongod; the NR feature"
            " bits in positions [ 1, 2 ] aren't recognized by this version of mongod",
            status.reason());
    }
}

TEST_F(DurableCatalogFeatureTrackerTest, DataFilesAreCompatibleWithRecognizedRepairableFeature) {
    getFeatureTracker()->setUsedNonRepairableFeaturesMaskForTestingOnly(0ULL);
    getFeatureTracker()->setUsedRepairableFeaturesMaskForTestingOnly(0ULL);

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
    }

    getFeatureTracker()->setUsedRepairableFeaturesMaskForTestingOnly(
        static_cast<RepairableFeatureMask>(kRepairableFeature1));

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature1);
            wuow.commit();
        }
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
    }
}

TEST_F(DurableCatalogFeatureTrackerTest,
       DataFilesAreIncompatibleWithAnUnrecognizedRepairableFeature) {
    getFeatureTracker()->setUsedNonRepairableFeaturesMaskForTestingOnly(0ULL);
    getFeatureTracker()->setUsedRepairableFeaturesMaskForTestingOnly(0ULL);

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
    }

    {
        auto opCtx = newOperationContext();
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature1);
            wuow.commit();
        }

        auto status = getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get());
        ASSERT_EQ(ErrorCodes::CanRepairToDowngrade, status.code());
        ASSERT_EQ(
            "The data files use features not recognized by this version of mongod; the R feature"
            " bits in positions [ 0 ] aren't recognized by this version of mongod",
            status.reason());
    }
}

TEST_F(DurableCatalogFeatureTrackerTest,
       DataFilesAreIncompatibleWithMultipleUnrecognizedRepairableFeatures) {
    getFeatureTracker()->setUsedNonRepairableFeaturesMaskForTestingOnly(0ULL);
    getFeatureTracker()->setUsedRepairableFeaturesMaskForTestingOnly(
        static_cast<RepairableFeatureMask>(kRepairableFeature1));

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get()));
    }

    {
        auto opCtx = newOperationContext();
        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature2);
            wuow.commit();
        }

        {
            WriteUnitOfWork wuow(opCtx.get());
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx.get(), kRepairableFeature3);
            wuow.commit();
        }

        auto status = getFeatureTracker()->isCompatibleWithCurrentCode(opCtx.get());
        ASSERT_EQ(ErrorCodes::CanRepairToDowngrade, status.code());
        ASSERT_EQ(
            "The data files use features not recognized by this version of mongod; the R feature"
            " bits in positions [ 1, 2 ] aren't recognized by this version of mongod",
            status.reason());
    }
}

}  // namespace
}  // namespace mongo
