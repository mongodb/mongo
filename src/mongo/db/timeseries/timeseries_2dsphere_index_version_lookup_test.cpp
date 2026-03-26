/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/timeseries/timeseries_2dsphere_index_version_lookup.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <optional>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

/** Builds a minimal ready index spec for a 2dsphere_bucket index on 'bucketKeyPath' (e.g.
 * data.loc). */
BSONObj make2dsphereBucketIndexSpec(StringData indexName,
                                    StringData bucketKeyPath,
                                    BSONObj extraFields = {}) {
    BSONObjBuilder bob;
    bob.append("v", 2);
    bob.append("key", BSON(bucketKeyPath << IndexNames::GEO_2DSPHERE_BUCKET));
    bob.append("name", indexName);
    bob.appendElements(extraFields);
    return bob.obj();
}

class Build2dsphereIndexVersionMapTest : public CatalogTestFixture {
public:
    OperationContext* opCtxV() {
        return operationContext();
    }

    const CollectionPtr& collPtr() const {
        return *_coll.get();
    }

    const Collection& collection() const {
        return *collPtr().get();
    }

    void createIndexAssertOk(const BSONObj& spec) {
        WriteUnitOfWork wuow(opCtxV());
        CollectionWriter writer{opCtxV(), _coll.get()};
        auto* writable = writer.getWritableCollection(opCtxV());
        auto sw =
            writable->getIndexCatalog()->createIndexOnEmptyCollection(opCtxV(), writable, spec);
        ASSERT_OK(sw);
        wuow.commit();
    }

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(opCtxV(), _nss, {}));
        _coll.emplace(opCtxV(), _nss, MODE_X);
    }

    void tearDown() override {
        _coll.reset();
        CatalogTestFixture::tearDown();
    }

    NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test", "ts_buckets");
    boost::optional<AutoGetCollection> _coll;
};

/**
 * Index specs with 2dsphereIndexVersion 4 are only accepted when the v4 feature flag is enabled
 * and FCV is new enough.
 */
class Build2dsphereIndexVersionMapV4Test : public Build2dsphereIndexVersionMapTest {
protected:
    void setUp() override {
        auto fcvSnap = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
        if (fcvSnap.isVersionInitialized()) {
            _fcvBefore = fcvSnap.getVersion();
        }
        _enableV4.emplace("featureFlag2dsphereIndexVersion4", true);
        // (Generic FCV reference): kLatest so getDefaultS2IndexVersion() is 4 and v4 bucket indexes
        // can be created in this test fixture.
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
        Build2dsphereIndexVersionMapTest::setUp();
    }

    void tearDown() override {
        Build2dsphereIndexVersionMapTest::tearDown();
        _enableV4.reset();
        if (_fcvBefore) {
            serverGlobalParams.mutableFCV.setVersion(*_fcvBefore);
        } else {
            serverGlobalParams.mutableFCV.reset();
        }
    }

private:
    std::optional<RAIIServerParameterControllerForTest> _enableV4;
    std::optional<multiversion::FeatureCompatibilityVersion> _fcvBefore;
};

TEST_F(Build2dsphereIndexVersionMapTest, EmptyCatalogReturnsEmptyMap) {
    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT(m.empty());
}

TEST_F(Build2dsphereIndexVersionMapTest, MapsDataFieldToVersion) {
    createIndexAssertOk(make2dsphereBucketIndexSpec(
        "loc_2dsphere", "data.loc"_sd, BSON(IndexDescriptor::k2dsphereVersionFieldName << 3)));

    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT_EQ(m.size(), 1U);
    auto it = m.find("loc");
    ASSERT(it != m.end());
    ASSERT_EQ(it->second, 3);
}

TEST_F(Build2dsphereIndexVersionMapTest, StripsDataPrefixForNestedPath) {
    createIndexAssertOk(make2dsphereBucketIndexSpec(
        "geo_2dsphere", "data.geo.sub"_sd, BSON(IndexDescriptor::k2dsphereVersionFieldName << 3)));

    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT_EQ(m.size(), 1U);
    auto it = m.find("geo.sub");
    ASSERT(it != m.end());
    ASSERT_EQ(it->second, 3);
}

TEST_F(Build2dsphereIndexVersionMapTest, CompoundIndexMapsOnly2dsphereBucketField) {
    BSONObj spec =
        BSON("v" << 2 << "key"
                 << BSON("data.ct" << 1 << "data.loc" << IndexNames::GEO_2DSPHERE_BUCKET) << "name"
                 << "cmp_ct_loc" << IndexDescriptor::k2dsphereVersionFieldName << 3);
    createIndexAssertOk(spec);

    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT_EQ(m.size(), 1U);
    ASSERT_EQ(m["loc"], 3);
}

TEST_F(Build2dsphereIndexVersionMapTest, MultipleIndexesAccumulateDistinctFields) {
    createIndexAssertOk(make2dsphereBucketIndexSpec(
        "a_geo", "data.a"_sd, BSON(IndexDescriptor::k2dsphereVersionFieldName << 3)));
    createIndexAssertOk(make2dsphereBucketIndexSpec(
        "b_geo", "data.b"_sd, BSON(IndexDescriptor::k2dsphereVersionFieldName << 3)));

    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT_EQ(m.size(), 2U);
    ASSERT_EQ(m["a"], 3);
    ASSERT_EQ(m["b"], 3);
}

TEST_F(Build2dsphereIndexVersionMapTest, Non2dsphereBucketKeySkipped) {
    BSONObj spec = BSON("v" << 2 << "key" << BSON("data.loc" << 1) << "name"
                            << "loc_1" << IndexDescriptor::k2dsphereVersionFieldName << 3);
    createIndexAssertOk(spec);

    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT(m.empty());
}

TEST_F(Build2dsphereIndexVersionMapTest, KeyNotUnderDataPrefixSkipped) {
    // Path must be "data.<userField>" for the map; a root-level geo field is ignored here.
    createIndexAssertOk(make2dsphereBucketIndexSpec(
        "root_geo", "loc"_sd, BSON(IndexDescriptor::k2dsphereVersionFieldName << 3)));

    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT(m.empty());
}

TEST_F(Build2dsphereIndexVersionMapV4Test, MapsDataFieldToVersion4) {
    createIndexAssertOk(make2dsphereBucketIndexSpec(
        "loc_2dsphere_v4", "data.loc"_sd, BSON(IndexDescriptor::k2dsphereVersionFieldName << 4)));

    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT_EQ(m.size(), 1U);
    ASSERT_EQ(m["loc"], 4);
}

TEST_F(Build2dsphereIndexVersionMapV4Test, CompoundIndexMaps2dsphereBucketFieldWithVersion4) {
    BSONObj spec =
        BSON("v" << 2 << "key"
                 << BSON("data.ct" << 1 << "data.loc" << IndexNames::GEO_2DSPHERE_BUCKET) << "name"
                 << "cmp_ct_loc_v4" << IndexDescriptor::k2dsphereVersionFieldName << 4);
    createIndexAssertOk(spec);

    auto m = timeseries::build2dsphereIndexVersionMap(collection());
    ASSERT_EQ(m.size(), 1U);
    ASSERT_EQ(m["loc"], 4);
}

}  // namespace
}  // namespace mongo
