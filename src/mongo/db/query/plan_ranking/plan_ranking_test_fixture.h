// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace plan_ranking {
class PlanRankingTestFixture : public CatalogTestFixture {
protected:
    PlanRankingTestFixture(NamespaceString nss) : nss(nss) {}

    void setUp() override;

    void insertDocuments(const std::vector<BSONObj>& docs);

    void insertNDocuments(int count);

    void tearDown() override;

    void createIndexOnEmptyCollection(OperationContext* opCtx,
                                      BSONObj index,
                                      std::string indexName,
                                      BSONObj options = BSONObj());

    std::pair<std::unique_ptr<CanonicalQuery>, PlannerData> createCQAndPlannerData(
        const MultipleCollectionAccessor& collections,
        BSONObj findFilter,
        std::function<void(FindCommandRequest&)> modifyFindCmd = [](FindCommandRequest&) {});

    MultipleCollectionAccessor getCollsAccessor();

    std::unique_ptr<DBDirectClient> client;
    boost::intrusive_ptr<ExpressionContext> expCtx;
    std::vector<IndexEntry> indices;
    NamespaceString nss;

private:
    CollectionAcquisition getCollection();
};
}  // namespace plan_ranking
}  // namespace mongo
