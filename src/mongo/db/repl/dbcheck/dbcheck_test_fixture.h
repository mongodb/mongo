// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bson_validate_gen.h"
#include "mongo/db/commands/dbcheck_command.h"
#include "mongo/db/repl/dbcheck/dbcheck.h"
#include "mongo/db/repl/dbcheck/dbcheck_gen.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/throttle_cursor.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>


namespace mongo {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.t");

const int64_t kDefaultMaxCount = std::numeric_limits<int64_t>::max();
const int64_t kDefaultMaxSize = std::numeric_limits<int64_t>::max();
const int64_t kDefaultMaxDocsPerBatch = 5000;
const int64_t kDefaultMaxBatchTimeMillis = 1000;

class DbCheckTest : public CatalogTestFixture {
public:
    DbCheckTest(Options options = {}, CollectionOptions collectionOptions = {})
        : CatalogTestFixture(std::move(options)),
          _collectionOptions(std::move(collectionOptions)) {}

    void setUp() override;

    /**
     *  Inserts 'numDocs' docs with _id values starting at 'startIDNum' and incrementing for each
     * document. Callers must avoid duplicate key insertions. These keys always contain a value in
     * the 'a' field. If `duplicateFieldNames` is true, the inserted doc will have a duplicated
     * field name so that it fails the kExtended mode of BSON validate check.
     */
    void insertDocs(OperationContext* opCtx,
                    int startIDNum,
                    int numDocs,
                    const std::vector<std::string>& fieldNames,
                    bool duplicateFieldNames = false);

    /**
     * Insert a document with an invalid UUID (incorrect length).
     */
    void insertInvalidUuid(OperationContext* opCtx,
                           int startIDNum,
                           const std::vector<std::string>& fieldNames);

    /**
     * Deletes 'numDocs' docs from kNss with _id values starting at 'startIDNum' and incrementing
     * for each document.
     */
    void deleteDocs(OperationContext* opCtx, int startIDNum, int numDocs);

    /**
     * Inserts documents without updating corresponding index tables to generate missing index
     * entries for the inserted documents.
     */
    void insertDocsWithMissingIndexKeys(OperationContext* opCtx,
                                        int startIDNum,
                                        int numDocs,
                                        const std::vector<std::string>& fieldNames);

    /**
     * Builds an index on kNss. 'indexKey' specifies the index key, e.g. {'a': 1};
     */
    void createIndex(OperationContext* opCtx, const BSONObj& indexKey);

    /**
     * Runs hashing and the missing keys check for kNss.
     */
    Status runHashForCollectionCheck(
        OperationContext* opCtx,
        const BSONObj& start,
        const BSONObj& end,
        boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParams,
        int64_t maxCount = std::numeric_limits<int64_t>::max(),
        int64_t maxBytes = std::numeric_limits<int64_t>::max(),
        Date_t deadlineOnSecondary = Date_t::max());

    /**
     * Runs hashing for the extra index keys check for kNss.
     */
    Status runHashForExtraIndexKeysCheck(
        OperationContext* opCtx,
        const BSONObj& batchStart,
        const BSONObj& batchEnd,
        const BSONObj& lastKeyChecked,
        boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParams,
        int64_t maxCount = std::numeric_limits<int64_t>::max(),
        int64_t maxBytes = std::numeric_limits<int64_t>::max(),
        Date_t deadlineOnSecondary = Date_t::max());

    /**
     *  Creates a secondary index check params struct to define the dbCheck operation.
     */
    SecondaryIndexCheckParameters createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum validateMode,
        std::string_view secondaryIndex,
        bool skipLookupForExtraKeys = false,
        BSONValidateModeEnum bsonValidateMode = BSONValidateModeEnum::kDefault);

    /**
     * Creates a DbCheckCollectionInfo struct.
     */
    DbCheckCollectionInfo createDbCheckCollectionInfo(OperationContext* opCtx,
                                                      const BSONObj& start,
                                                      const BSONObj& end,
                                                      const SecondaryIndexCheckParameters& params);

    /**
     * Fetches the number of entries in the health log that match the given query.
     */
    int getNumDocsFoundInHealthLog(OperationContext* opCtx, const BSONObj& query);

protected:
    CollectionOptions _collectionOptions;
};

class DbCheckClusteredCollectionTest : public DbCheckTest {
public:
    DbCheckClusteredCollectionTest(Options options = {}) : DbCheckTest(std::move(options)) {
        _collectionOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    }
};

const auto docMinKey = BSON("_id" << MINKEY);
const auto docMaxKey = BSON("_id" << MAXKEY);
const auto aIndexMinKey = BSON("a" << MINKEY);
const auto aIndexMaxKey = BSON("a" << MAXKEY);

const auto errQuery = BSON(HealthLogEntry::kSeverityFieldName << "error");
const auto missingKeyQuery =
    BSON(HealthLogEntry::kSeverityFieldName << "error" << HealthLogEntry::kMsgFieldName
                                            << "Document has missing index keys");
const auto idRecordIdMismatchQuery =
    BSON(HealthLogEntry::kSeverityFieldName
         << "error" << HealthLogEntry::kMsgFieldName
         << "Document's _id mismatches its RecordId in clustered collection");
const auto warningQuery = BSON(HealthLogEntry::kSeverityFieldName << "warning");
const auto BSONWarningQuery =
    BSON(HealthLogEntry::kSeverityFieldName << "warning" << HealthLogEntry::kMsgFieldName
                                            << "Document is not well-formed BSON");
}  // namespace mongo
