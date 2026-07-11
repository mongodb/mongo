// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

// TODO: SERVER-88503 Remove Index Filters feature.

/**
 * DB commands for index filters.
 * Index filter commands work on a different data structure in the collection
 * info cache from the plan cache.
 * The user still thinks of index filter commands as part of the plan cache functionality
 * so the command name prefix is still "planCache".
 *
 * These are in a header to facilitate unit testing. See index_filter_commands_test.cpp.
 */

/**
 * IndexFilterCommand
 * Defines common attributes for all index filter related commands
 * such as slaveOk.
 */
class IndexFilterCommand : public BasicCommand {
public:
    IndexFilterCommand(const std::string& name, const std::string& helpText);

    /**
     * Entry point from command subsystem.
     * Implementation provides standardization of error handling
     * such as adding error code and message to BSON result.
     *
     * Do not override in derived classes.
     * Override runPlanCacheCommands instead to
     * implement plan cache command functionality.
     */

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override;

    bool supportsWriteConcern(const BSONObj& cmd) const override;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override;

    std::string help() const override;

    /**
     * One action type defined for index filter commands:
     * - planCacheIndexFilter
     */
    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override;

    virtual Status runIndexFilterCommand(OperationContext* opCtx,
                                         const CollectionPtr& collection,
                                         const BSONObj& cmdObj,
                                         BSONObjBuilder* bob) = 0;

private:
    std::string helpText;
};

/**
 * ListFilters
 *
 * { planCacheListFilters: <collection> }
 *
 */
class ListFilters : public IndexFilterCommand {
public:
    ListFilters();

    Status runIndexFilterCommand(OperationContext* opCtx,
                                 const CollectionPtr& collection,
                                 const BSONObj& cmdObj,
                                 BSONObjBuilder* bob) override;

    /**
     * Looks up index filters from collection's query settings.
     * Inserts index filters into BSON builder.
     */
    static Status list(const QuerySettings& querySettings, BSONObjBuilder* bob);
};

/**
 * ClearFilters
 *
 * { planCacheClearFilters: <collection>, query: <query>, sort: <sort>, projection: <projection> }
 *
 */
class ClearFilters : public IndexFilterCommand {
public:
    ClearFilters();

    Status runIndexFilterCommand(OperationContext* opCtx,
                                 const CollectionPtr& collection,
                                 const BSONObj& cmdObj,
                                 BSONObjBuilder* bob) override;

    /**
     * Removes corresponding entries from plan caches. If query shape is provided, clears index
     * filter for the query. Otherwise, clears collection's filters.
     */
    static Status clear(OperationContext* opCtx,
                        const CollectionPtr& collection,
                        const BSONObj& cmdObj,
                        QuerySettings* querySettings,
                        PlanCache* planCacheClassic,
                        sbe::PlanCache* planCacheSBE);
};

/**
 * SetFilter
 *
 * {
 *     planCacheSetFilter: <collection>,
 *     query: <query>,
 *     sort: <sort>,
 *     projection: <projection>,
 *     indexes: [ <index1>, <index2>, <index3>, ... ]
 * }
 *
 */
class SetFilter : public IndexFilterCommand {
public:
    SetFilter();

    Status runIndexFilterCommand(OperationContext* opCtx,
                                 const CollectionPtr& collection,
                                 const BSONObj& cmdObj,
                                 BSONObjBuilder* bob) override;

    /**
     * Sets index filter for a query shape. Removes entries for the query shape from plan cache.
     */
    static Status set(OperationContext* opCtx,
                      const CollectionPtr& collection,
                      const BSONObj& cmdObj,
                      QuerySettings* querySettings,
                      PlanCache* planCacheClassic,
                      sbe::PlanCache* planCacheSBE);
};

}  // namespace mongo
