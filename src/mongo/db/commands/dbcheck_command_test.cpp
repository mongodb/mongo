// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/dbcheck/dbcheck_test_fixture.h"
#include "mongo/db/repl/dbcheck/health_log.h"
#include "mongo/db/repl/dbcheck/health_log_gen.h"
#include "mongo/db/repl/dbcheck/health_log_interface.h"

#include <boost/optional/optional.hpp>

namespace mongo {

TEST_F(DbCheckTest, BasicDbCheck) {
    auto opCtx = operationContext();
    std::vector<std::string> fieldNames = {"a"};

    createIndex(opCtx, BSON("a" << 1));
    insertDocs(opCtx, 0, 10, fieldNames);

    auto secondaryIndexCheckParams = createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck,
        "" /* secondaryIndex */);
    auto collInfo =
        createDbCheckCollectionInfo(opCtx, docMinKey, docMaxKey, secondaryIndexCheckParams);
    DbChecker dbChecker(collInfo);

    // Run the dbChecker.
    dbChecker.doCollection(opCtx);

    // Shut down the health log writer so that the writes get flushed to the health log collection.
    auto service = getServiceContext();
    HealthLogInterface::get(service)->shutdown();

    // Verify that no error health log entries were logged.
    ASSERT_EQ(0, getNumDocsFoundInHealthLog(opCtx, errQuery));
}

}  // namespace mongo
