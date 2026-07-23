/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/db/catalog/catalog_control.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class FeatureCompatibilityVersionTestFixture : public CatalogTestFixture {
    void setUp() override {
        CatalogTestFixture::setUp();

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(),
                                                                            repl::ReplSettings());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
        // Unit test framework sets FCV to latest. Reset it to test FCV initialization logic.
        serverGlobalParams.mutableFCV.reset();
    }

protected:
    void doStartupFCVSequence() {
        Lock::GlobalWrite lock(operationContext());
        FeatureCompatibilityVersion::setIfCleanStartup(operationContext(), storageInterface());
        FeatureCompatibilityVersion::initializeForStartup(operationContext());
        FeatureCompatibilityVersion::fassertInitializedAfterStartup(operationContext());
    }
};

TEST_F(FeatureCompatibilityVersionTestFixture, CanInitFCVWithIncompleteForegroundIndexBuild) {
    // Create an incomplete index build in the catalog for a collection in the admin database
    {
        const auto nss =
            NamespaceString::createNamespaceString_forTest("admin.incompleteForegroundIndexBuild");
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest{nss,
                                         AcquisitionPrerequisites::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite},
            MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), &coll};
        auto writableColl = writer.getWritableCollection(operationContext());
        IndexDescriptor desc{IndexNames::BTREE,
                             BSON("v" << 2 << "name"
                                      << "x_1"
                                      << "key" << BSON("x" << 1))};
        ASSERT_OK(
            writableColl->prepareForIndexBuild(operationContext(), &desc, boost::none, false));
        wuow.commit();
    }

    // Simulate the startup path by closing the catalog and reopening it without reconciling (as
    // that happens after FCV is initialized). This would fail if we tried to initialize the entire
    // admin db as a non-fcv collection is in an invalid state.
    Lock::GlobalLock globalLk(operationContext(), MODE_X);
    catalog::closeCatalog(operationContext());

    auto* storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    storageEngine->loadCatalog(
        operationContext(), boost::none, StorageEngine::LastShutdownState::kClean);
    FeatureCompatibilityVersion::initializeForStartup(operationContext());
}

TEST_F(FeatureCompatibilityVersionTestFixture,
       FindFeatureCompatibilityVersionDocumentDoesNotRequireIndex) {
    doStartupFCVSequence();

    // Remove the _id index from the collection to verify that FCV lookup doesn't rely on it
    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest{NamespaceString::kServerConfigurationNamespace,
                                         AcquisitionPrerequisites::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite},
            MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), &coll};
        auto writableColl = writer.getWritableCollection(operationContext());
        ASSERT_TRUE(writableColl->isIndexPresent("_id_"));
        writableColl->removeIndex(operationContext(), "_id_");
        ASSERT_FALSE(writableColl->isIndexPresent("_id_"));
        wuow.commit();
    }

    ASSERT_OK(
        FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(operationContext()));
}

}  // namespace
}  // namespace mongo
