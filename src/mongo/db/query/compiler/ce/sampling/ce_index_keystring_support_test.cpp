// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/ce_index_keystring_support.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/index_builds/index_build_test_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_test_utils.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <functional>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::ce {
namespace {

using namespace cost_based_ranker;

using IndexKeystringSamplerFixture = ServiceContextMongoDTest;

CollectionAcquisition acquireForRead(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

class IndexKeystringSamplerTest : public IndexKeystringSamplerFixture {
public:
    void setUp() override {
        IndexKeystringSamplerFixture::setUp();
        _opCtxRaii = cc().makeOperationContext();
        opCtx = _opCtxRaii.get();
    }

    void tearDown() override {
        sampler.reset();
        indexEntry.reset();
        _acq.reset();
        _opCtxRaii.reset();
        opCtx = nullptr;
        IndexKeystringSamplerFixture::tearDown();
    }

    const CollectionPtr& coll() const {
        return _acq->getCollectionPtr();
    }

    void setupIndex(const NamespaceString& nss,
                    const BSONObj& indexSpec,
                    const std::function<void(IndexEntry&)>& customizeEntry = {}) {
        createCollAndInsertDocuments(opCtx, nss, {BSON("_id" << 0)});
        ASSERT_OK(createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));
        _acq = acquireForRead(opCtx, nss);
        indexEntry = makeIndexEntry(opCtx, coll(), indexSpec["name"].str());
        if (customizeEntry) {
            customizeEntry(*indexEntry);
        }
        sampler.emplace(opCtx, coll(), *indexEntry);
    }

    OperationContext* opCtx = nullptr;
    boost::optional<IndexEntry> indexEntry;
    boost::optional<IndexKeystringSampler> sampler;

private:
    ServiceContext::UniqueOperationContext _opCtxRaii;
    boost::optional<CollectionAcquisition> _acq;
};

TEST_F(IndexKeystringSamplerFixture, RequiresKeystringEstimation) {
    ASSERT_FALSE(requiresKeystringEstimation(nullptr));

    auto btreeEntry = buildSimpleIndexEntry(BSON("a" << 1));
    ASSERT_FALSE(requiresKeystringEstimation(&btreeEntry));

    {
        auto opCtx = cc().makeOperationContext();
        QueryKnobGuardForTest guard{
            opCtx.get(), "internalQuerySamplingCEForceKeystringEstimation", true};
        ASSERT_TRUE(requiresKeystringEstimation(&btreeEntry));
        ASSERT_FALSE(requiresKeystringEstimation(nullptr));
    }

    // Any non-simple collator requires keystring estimation, regardless of index type.
    auto statusWithCollator = CollatorFactoryInterface::get(getServiceContext())
                                  ->makeFromBSON(BSON("locale" << "en"
                                                               << "strength" << 2));
    ASSERT_OK(statusWithCollator.getStatus());
    auto collatedEntry = buildSimpleIndexEntry(BSON("a" << 1));
    collatedEntry.collator = statusWithCollator.getValue().get();
    ASSERT_TRUE(requiresKeystringEstimation(&collatedEntry));

    auto hashedEntry = buildSimpleIndexEntry(BSON("a" << "hashed"));
    ASSERT_TRUE(requiresKeystringEstimation(&hashedEntry));
    auto wildcardEntry = buildSimpleIndexEntry(BSON("$**" << 1));
    ASSERT_TRUE(requiresKeystringEstimation(&wildcardEntry));
}

TEST_F(IndexKeystringSamplerTest, CountNDVKeyStringWildcard) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.CountNDVKeyStringWildcard");
    setupIndex(nss,
               BSON("name" << "$**_1" << "key" << BSON("$**" << 1) << "v" << 2),
               [](IndexEntry& indexEntry) {
                   indexEntry.keyPattern = BSON("$_path" << 1 << "a" << 1);
                   indexEntry.wildcardFieldPos = 1;
               });

    std::vector<BSONObj> sample = {BSON("_id" << 0 << "a" << 0 << "b" << 0),
                                   BSON("_id" << 1 << "a" << 1),
                                   BSON("_id" << 2 << "a" << 0),
                                   BSON("_id" << 3)};

    IndexBounds bounds = makePointIntervalBounds(/*str=*/"a", /*fieldName=*/"$_path");
    auto counts = sampler->countNDV(
        {{"$_path"}}, sample, std::span<const OrderedIntervalList>(bounds.fields));
    ASSERT_EQ(counts.totalSampleKeys, 4UL);
    ASSERT_EQ(counts.uniqueMatchingKeys, 3UL);
    ASSERT_EQ(counts.sampleUniqueKeys, 1UL);

    counts = sampler->countNDV(
        {{"$_path"}, {"a"}}, sample, std::span<const OrderedIntervalList>(bounds.fields));
    ASSERT_EQ(counts.totalSampleKeys, 4UL);
    ASSERT_EQ(counts.uniqueMatchingKeys, 3UL);
    ASSERT_EQ(counts.sampleUniqueKeys, 2UL);
}

TEST_F(IndexKeystringSamplerTest, CountNDVKeyStringHashed) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.CountNDVKeyStringSkipField");
    setupIndex(nss,
               BSON("name" << "a_hashed" << "key" << BSON("a" << "hashed" << "b" << 1 << "c" << 1)
                           << "v" << 2));

    std::vector<BSONObj> sample = {BSON("_id" << 0 << "a" << 0 << "b" << 0 << "c" << 0),
                                   BSON("_id" << 1 << "a" << 1 << "b" << 1 << "c" << 0),
                                   BSON("_id" << 2 << "a" << 1 << "b" << 2 << "c" << 1),
                                   BSON("_id" << 3 << "a" << 1.1 << "b" << 3 << "c" << 0)};
    auto counts = sampler->countNDV({{"a"}, {"c"}}, sample);
    ASSERT_EQ(counts.totalSampleKeys, 4UL);
    ASSERT_EQ(counts.uniqueMatchingKeys, 4UL);
    ASSERT_EQ(counts.sampleUniqueKeys, 3UL);
}

}  // namespace
}  // namespace mongo::ce
