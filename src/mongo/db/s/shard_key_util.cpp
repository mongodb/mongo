/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/s/shard_key_util.h"

#include <absl/container/node_hash_map.h>
#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <algorithm>
#include <iterator>
#include <list>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/list_indexes_gen.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace shardkeyutil {
namespace {

constexpr StringData kCheckShardingIndexCmdName = "checkShardingIndex"_sd;
constexpr StringData kKeyPatternField = "keyPattern"_sd;

/**
 * Create an index specification used for create index command. It is the responsibility of the
 * caller to ensure the index key, namespace, and parameters passed in are valid.
 */
BSONObj makeIndexSpec(const NamespaceString& nss,
                      const BSONObj& keys,
                      const BSONObj& collation,
                      bool unique,
                      boost::optional<TimeseriesOptions> tsOpts) {
    BSONObjBuilder index;

    // Required fields for an index.
    index.append("key", keys);

    StringBuilder indexName;
    auto keysToIterate = keys;

    // Time-series indexes are named using the meta and timeFields defined by the user and not the
    // field names of the buckets collection. To match unsharded time-series collections, the index
    // name must be generated using the index key for the view namespace.
    //
    // Unsharded collections can appear with the key '{_id:1}'. We will return the key as is in
    // that case.
    if (tsOpts && !(keys.hasField(timeseries::kBucketIdFieldName))) {
        boost::optional<BSONObj> bucketKeysForName =
            timeseries::createTimeseriesIndexFromBucketsIndexSpec(*tsOpts, keys);
        tassert(7711204,
                str::stream() << "Invalid index backing a shard key on a time-series collection: "
                              << keys,
                bucketKeysForName.has_value());
        keysToIterate = bucketKeysForName->getOwned();
    }
    bool isFirstKey = true;
    for (BSONObjIterator keyIter(keysToIterate); keyIter.more();) {
        BSONElement currentKey = keyIter.next();

        if (isFirstKey) {
            isFirstKey = false;
        } else {
            indexName << "_";
        }

        indexName << currentKey.fieldName() << "_";
        if (currentKey.isNumber()) {
            indexName << currentKey.numberInt();
        } else {
            indexName << currentKey.str();  // This should match up with shell command.
        }
    }
    index.append("name", indexName.str());

    // Index options.
    if (!collation.isEmpty()) {
        // Creating an index with the "collation" option requires a v=2 index.
        index.append("v", static_cast<int>(IndexDescriptor::IndexVersion::kV2));
        index.append("collation", collation);
    }

    if (unique && !IndexDescriptor::isIdIndexPattern(keys)) {
        index.appendBool("unique", unique);
    }

    return index.obj();
}

/**
 * Constructs the BSON specification document for the create indexes command using the given
 * namespace, index key and options.
 */
BSONObj makeCreateIndexesCmd(const NamespaceString& nss,
                             const BSONObj& keys,
                             const BSONObj& collation,
                             bool unique,
                             boost::optional<TimeseriesOptions> tsOpts) {
    auto indexSpec = makeIndexSpec(nss, keys, collation, unique, tsOpts);
    // The outer createIndexes command.
    BSONObjBuilder createIndexes;
    createIndexes.append("createIndexes", nss.coll());
    createIndexes.append("indexes", BSON_ARRAY(indexSpec));
    createIndexes.append("writeConcern", WriteConcernOptions::Majority);
    return createIndexes.obj();
}

}  // namespace

bool validShardKeyIndexExists(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const ShardKeyPattern& shardKeyPattern,
                              const boost::optional<BSONObj>& defaultCollation,
                              bool requiresUnique,
                              const ShardKeyValidationBehaviors& behaviors,
                              std::string* errMsg) {
    auto indexes = behaviors.loadIndexes(nss);

    // 1.  Verify consistency with existing unique indexes
    for (const auto& idx : indexes) {
        BSONObj currentKey = idx["key"].embeddedObject();
        bool isUnique = idx["unique"].trueValue();
        bool isPrepareUnique = idx["prepareUnique"].trueValue();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "can't shard collection '" << nss.toStringForErrorMsg()
                              << "' with unique index on " << currentKey
                              << " and proposed shard key " << shardKeyPattern.toBSON()
                              << ". Uniqueness can't be maintained unless shard key is a prefix",
                (!isUnique && !isPrepareUnique) ||
                    shardKeyPattern.isIndexUniquenessCompatible(currentKey));
    }

    // 2. Check for a useful index
    bool hasUsefulIndexForKey = false;
    std::string allReasons;
    for (const auto& idx : indexes) {
        std::string reasons;
        BSONObj currentKey = idx["key"].embeddedObject();
        // Check 2.i. and 2.ii.
        if (!idx["sparse"].trueValue() && idx["filter"].eoo() && idx["collation"].eoo() &&
            shardKeyPattern.toBSON().isPrefixOf(currentKey,
                                                SimpleBSONElementComparator::kInstance)) {
            // We can't currently use hashed indexes with a non-default hash seed
            // Check iv.
            // Note that this means that, for sharding, we only support one hashed index
            // per field per collection.
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "can't shard collection " << nss.toStringForErrorMsg()
                                  << " with hashed shard key " << shardKeyPattern.toBSON()
                                  << " because the hashed index uses a non-default seed of "
                                  << idx["seed"].numberInt(),
                    !shardKeyPattern.isHashedPattern() || idx["seed"].eoo() ||
                        idx["seed"].numberInt() == BSONElementHasher::DEFAULT_HASH_SEED);
            hasUsefulIndexForKey = true;
        }
        if (idx["sparse"].trueValue()) {
            reasons += " Index key is sparse.";
        }
        if (idx["filter"].ok()) {
            reasons += " Index key is partial.";
        }
        if (idx["collation"].ok()) {
            reasons += " Index has a non-simple collation.";
        }
        if (!reasons.empty()) {
            allReasons =
                " Index " + idx["name"] + " cannot be used for sharding because [" + reasons + " ]";
        }
    }

    // 3. If proposed key is required to be unique, additionally check for exact match.
    if (hasUsefulIndexForKey && requiresUnique) {
        BSONObj eqQuery =
            BSON("ns" << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                      << "key" << shardKeyPattern.toBSON());
        BSONObj eqQueryResult;

        for (const auto& idx : indexes) {
            if (SimpleBSONObjComparator::kInstance.evaluate(idx["key"].embeddedObject() ==
                                                            shardKeyPattern.toBSON())) {
                eqQueryResult = idx;
                break;
            }
        }

        if (eqQueryResult.isEmpty()) {
            // If no exact match, index not useful, but still possible to create one later
            hasUsefulIndexForKey = false;
        } else {
            bool isExplicitlyUnique = eqQueryResult["unique"].trueValue();
            BSONObj currKey = eqQueryResult["key"].embeddedObject();
            bool isCurrentID = (currKey.firstElementFieldNameStringData() == "_id");
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "can't shard collection " << nss.toStringForErrorMsg() << ", "
                                  << shardKeyPattern.toBSON()
                                  << " index not unique, and unique index explicitly specified",
                    isExplicitlyUnique || isCurrentID);
        }
    }

    if (errMsg && !allReasons.empty()) {
        *errMsg += allReasons;
    }

    if (hasUsefulIndexForKey) {
        // Check 2.iii Make sure that there is a useful, non-multikey index available.
        behaviors.verifyUsefulNonMultiKeyIndex(nss, shardKeyPattern.toBSON());
    }

    return hasUsefulIndexForKey;
}

bool validateShardKeyIndexExistsOrCreateIfPossible(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const ShardKeyPattern& shardKeyPattern,
                                                   const boost::optional<BSONObj>& defaultCollation,
                                                   bool unique,
                                                   bool enforceUniquenessCheck,
                                                   const ShardKeyValidationBehaviors& behaviors,
                                                   boost::optional<TimeseriesOptions> tsOpts,
                                                   bool updatedToHandleTimeseriesIndex) {
    std::string errMsg;
    if (validShardKeyIndexExists(opCtx,
                                 nss,
                                 shardKeyPattern,
                                 defaultCollation,
                                 unique && enforceUniquenessCheck,
                                 behaviors,
                                 &errMsg)) {
        return false;
    }

    // 4. If no useful index, verify we can create one.
    behaviors.verifyCanCreateShardKeyIndex(nss, &errMsg);

    // 5. If the shard key index is on a buckets namespace, we need to convert the shard key index.
    // We only do this if the DDL coordinator is running on FCV > 7.3. Previous versions should fall
    // back on the original index created.
    //
    // TODO (SERVER-79304): Remove 'updatedToHandleTimeseriesIndex' once 8.0 becomes last LTS, or
    // update the ticket number to when the parameter can be removed.
    auto indexKeyPatternBSON = shardKeyPattern.toBSON();
    if (updatedToHandleTimeseriesIndex && tsOpts.has_value()) {
        // 'createBucketsShardKeyIndexFromTimeseriesShardKeySpec' expects the shard key to be
        // already "buckets-encoded". For example, shard keys on the timeField should already be
        // changed to use the "control.min.<timeField>". If the shard key is not buckets
        // encoded, the function will return 'boost::none'.
        boost::optional<BSONObj> bucketShardKeyIndexPattern =
            timeseries::createBucketsShardKeyIndexFromBucketsShardKeySpec(*tsOpts,
                                                                          shardKeyPattern.toBSON());
        tassert(7711202,
                str::stream() << "Invalid index backing a shard key on a time-series collection:  "
                              << indexKeyPatternBSON,
                bucketShardKeyIndexPattern.has_value());
        indexKeyPatternBSON = bucketShardKeyIndexPattern->getOwned();
    }

    // 6. If no useful index exists and we can create one based on the proposed shard key. We only
    // need to call ensureIndex on primary shard, since indexes get copied to receiving shard
    // whenever a migrate occurs. If the collection has a default collation, explicitly send the
    // simple collation as part of the createIndex request.

    // We will only pass 'tsOpts' if 'updatedToHandleTimeseriesIndex' is true, and thus we are
    // making the updated time-series index for the sharding DDL commands.
    behaviors.createShardKeyIndex(nss,
                                  indexKeyPatternBSON,
                                  defaultCollation,
                                  unique,
                                  updatedToHandleTimeseriesIndex ? tsOpts : boost::none);
    return true;
}

// TODO: SERVER-64187 move calls to validateShardKeyIsNotEncrypted into
// validateShardKeyIndexExistsOrCreateIfPossible
void validateShardKeyIsNotEncrypted(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const ShardKeyPattern& shardKeyPattern) {
    AutoGetCollection collection(
        opCtx,
        nss,
        MODE_IS,
        AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));
    if (!collection || collection.getView()) {
        return;
    }

    const auto& encryptConfig = collection->getCollectionOptions().encryptedFieldConfig;
    if (!encryptConfig) {
        // this collection is not encrypted
        return;
    }

    auto& encryptedFields = encryptConfig->getFields();
    std::vector<FieldRef> encryptedFieldRefs;
    std::transform(encryptedFields.begin(),
                   encryptedFields.end(),
                   std::back_inserter(encryptedFieldRefs),
                   [](auto& path) { return FieldRef(path.getPath()); });

    for (const auto& keyFieldRef : shardKeyPattern.getKeyPatternFields()) {
        auto match = findMatchingEncryptedField(*keyFieldRef, encryptedFieldRefs);
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Sharding is not allowed on keys that are equal to, or a "
                                 "prefix of, the encrypted field "
                              << match->encryptedField.dottedField(),
                !match || !match->keyIsPrefixOrEqual);
        uassert(
            ErrorCodes::InvalidOptions,
            str::stream() << "Sharding is not allowed on keys whose prefix is the encrypted field "
                          << match->encryptedField.dottedField(),
            !match || match->keyIsPrefixOrEqual);
    }
}

std::vector<BSONObj> ValidationBehaviorsShardCollection::loadIndexes(
    const NamespaceString& nss) const {
    const bool includeBuildUUIDs = false;
    const int options = 0;
    std::list<BSONObj> indexes = _localClient->getIndexSpecs(nss, includeBuildUUIDs, options);
    // Convert std::list to a std::vector.
    return std::vector<BSONObj>{std::make_move_iterator(std::begin(indexes)),
                                std::make_move_iterator(std::end(indexes))};
}

void ValidationBehaviorsShardCollection::verifyUsefulNonMultiKeyIndex(
    const NamespaceString& nss, const BSONObj& proposedKey) const {
    auto res = Shard::CommandResponse::getEffectiveStatus(_dataShard->runCommand(
        _opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        DatabaseName::kAdmin,
        BSON(kCheckShardingIndexCmdName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
             << kKeyPatternField << proposedKey),
        Shard::RetryPolicy::kIdempotent));
    uassert(ErrorCodes::InvalidOptions, res.reason(), res.isOK());
}

void ValidationBehaviorsShardCollection::verifyCanCreateShardKeyIndex(const NamespaceString& nss,
                                                                      std::string* errMsg) const {
    repl::ReadConcernArgs readConcern =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    FindCommandRequest findCommand(nss);
    findCommand.setLimit(1);
    findCommand.setReadConcern(readConcern.toBSONInner());
    Shard::QueryResponse response = uassertStatusOK(
        _dataShard->runExhaustiveCursorCommand(_opCtx,
                                               ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                               nss.dbName(),
                                               findCommand.toBSON(BSONObj()),
                                               Milliseconds(-1)));
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Please create an index that starts with the proposed shard key before"
                             " sharding the collection. "
                          << *errMsg,
            response.docs.empty());
}

void ValidationBehaviorsShardCollection::createShardKeyIndex(
    const NamespaceString& nss,
    const BSONObj& proposedKey,
    const boost::optional<BSONObj>& defaultCollation,
    bool unique,
    boost::optional<TimeseriesOptions> tsOpts) const {
    BSONObj collation =
        defaultCollation && !defaultCollation->isEmpty() ? CollationSpec::kSimpleSpec : BSONObj();
    auto createIndexesCmd = makeCreateIndexesCmd(nss, proposedKey, collation, unique, tsOpts);
    BSONObj res;
    _localClient->runCommand(nss.dbName(), createIndexesCmd, res);
    uassertStatusOK(getStatusFromCommandResult(res));
}

ValidationBehaviorsRefineShardKey::ValidationBehaviorsRefineShardKey(OperationContext* opCtx,
                                                                     const NamespaceString& nss)
    : _opCtx(opCtx),
      _cri(uassertStatusOK(
          Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                       nss))),
      _indexShard(uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(
          opCtx, _cri.cm.getMinKeyShardIdWithSimpleCollation()))) {}

std::vector<BSONObj> ValidationBehaviorsRefineShardKey::loadIndexes(
    const NamespaceString& nss) const {
    auto indexesRes = _indexShard->runExhaustiveCursorCommand(
        _opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        nss.dbName(),
        appendShardVersion(BSON("listIndexes" << nss.coll()),
                           _cri.getShardVersion(_indexShard->getId())),
        Milliseconds(-1));
    if (indexesRes.getStatus().code() != ErrorCodes::NamespaceNotFound) {
        return uassertStatusOK(indexesRes).docs;
    }
    return {};
}

void ValidationBehaviorsRefineShardKey::verifyUsefulNonMultiKeyIndex(
    const NamespaceString& nss, const BSONObj& proposedKey) const {
    auto checkShardingIndexRes = uassertStatusOK(_indexShard->runCommand(
        _opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        DatabaseName::kAdmin,
        appendShardVersion(
            BSON(kCheckShardingIndexCmdName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                 << kKeyPatternField << proposedKey),
            _cri.getShardVersion(_indexShard->getId())),
        Shard::RetryPolicy::kIdempotent));
    if (checkShardingIndexRes.commandStatus == ErrorCodes::UnknownError) {
        // CheckShardingIndex returns UnknownError if a compatible shard key index cannot be found,
        // but we return InvalidOptions to correspond with the shardCollection behavior.
        uasserted(ErrorCodes::InvalidOptions, checkShardingIndexRes.response["errmsg"].str());
    }
    // Rethrow any other error to allow retries on retryable errors.
    uassertStatusOK(checkShardingIndexRes.commandStatus);
}

void ValidationBehaviorsRefineShardKey::verifyCanCreateShardKeyIndex(const NamespaceString& nss,
                                                                     std::string* errMsg) const {
    uasserted(
        ErrorCodes::InvalidOptions,
        str::stream() << "Please create an index that starts with the proposed shard key before"
                         " refining the collection's shard key. "
                      << *errMsg);
}

void ValidationBehaviorsRefineShardKey::createShardKeyIndex(
    const NamespaceString& nss,
    const BSONObj& proposedKey,
    const boost::optional<BSONObj>& defaultCollation,
    bool unique,
    boost::optional<TimeseriesOptions> tsOpts) const {
    MONGO_UNREACHABLE;
}

ValidationBehaviorsLocalRefineShardKey::ValidationBehaviorsLocalRefineShardKey(
    OperationContext* opCtx, const CollectionPtr& coll)
    : _opCtx(opCtx), _coll(coll) {}


std::vector<BSONObj> ValidationBehaviorsLocalRefineShardKey::loadIndexes(
    const NamespaceString& nss) const {
    std::vector<BSONObj> indexes;
    auto it = _coll->getIndexCatalog()->getIndexIterator(
        _opCtx, mongo::IndexCatalog::InclusionPolicy::kReady);
    while (it->more()) {
        auto entry = it->next();
        indexes.push_back(entry->descriptor()->toBSON());
    }
    return indexes;
}

void ValidationBehaviorsLocalRefineShardKey::verifyUsefulNonMultiKeyIndex(
    const NamespaceString& nss, const BSONObj& proposedKey) const {
    std::string tmpErrMsg = "couldn't find valid index for shard key";
    uassert(ErrorCodes::InvalidOptions,
            tmpErrMsg,
            findShardKeyPrefixedIndex(_opCtx,
                                      _coll,
                                      proposedKey,
                                      /*requireSingleKey=*/true,
                                      &tmpErrMsg));
}

void ValidationBehaviorsLocalRefineShardKey::verifyCanCreateShardKeyIndex(
    const NamespaceString& nss, std::string* errMsg) const {
    uasserted(
        ErrorCodes::InvalidOptions,
        str::stream() << "Please create an index that starts with the proposed shard key before"
                         " refining the collection's shard key. "
                      << *errMsg);
}

void ValidationBehaviorsLocalRefineShardKey::createShardKeyIndex(
    const NamespaceString& nss,
    const BSONObj& proposedKey,
    const boost::optional<BSONObj>& defaultCollation,
    bool unique,
    boost::optional<TimeseriesOptions> tsOpts) const {
    MONGO_UNREACHABLE;
}

ValidationBehaviorsReshardingBulkIndex::ValidationBehaviorsReshardingBulkIndex()
    : _opCtx(nullptr), _cloneTimestamp(), _shardKeyIndexSpec() {}

std::vector<BSONObj> ValidationBehaviorsReshardingBulkIndex::loadIndexes(
    const NamespaceString& nss) const {
    invariant(_opCtx);
    auto grid = Grid::get(_opCtx);
    // This is a hack to make this code work in resharding_recipient_service_test. In real
    // deployment, grid and catalogCache should always exist.
    if (!grid->isInitialized() || !grid->catalogCache()) {
        return std::vector<BSONObj>();
    }
    auto catalogCache = grid->catalogCache();
    auto cri = catalogCache->getTrackedCollectionRoutingInfo(_opCtx, nss);
    auto [indexSpecs, _] = MigrationDestinationManager::getCollectionIndexes(
        _opCtx, nss, cri.cm.getMinKeyShardIdWithSimpleCollation(), cri, _cloneTimestamp);
    return indexSpecs;
}

void ValidationBehaviorsReshardingBulkIndex::verifyUsefulNonMultiKeyIndex(
    const NamespaceString& nss, const BSONObj& proposedKey) const {
    invariant(_opCtx);
    auto catalogCache = Grid::get(_opCtx)->catalogCache();
    auto cri = catalogCache->getTrackedCollectionRoutingInfo(_opCtx, nss);
    auto shard = uassertStatusOK(Grid::get(_opCtx)->shardRegistry()->getShard(
        _opCtx, cri.cm.getMinKeyShardIdWithSimpleCollation()));
    auto checkShardingIndexRes = uassertStatusOK(shard->runCommand(
        _opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        DatabaseName::kAdmin,
        appendShardVersion(
            BSON(kCheckShardingIndexCmdName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                 << kKeyPatternField << proposedKey),
            cri.getShardVersion(shard->getId())),
        Shard::RetryPolicy::kIdempotent));
    if (checkShardingIndexRes.commandStatus == ErrorCodes::UnknownError) {
        // CheckShardingIndex returns UnknownError if a compatible shard key index cannot be found,
        // but we return InvalidOptions to correspond with the shardCollection behavior.
        uasserted(ErrorCodes::InvalidOptions, checkShardingIndexRes.response["errmsg"].str());
    }
    // Rethrow any other error to allow retries on retryable errors.
    uassertStatusOK(checkShardingIndexRes.commandStatus);
}

void ValidationBehaviorsReshardingBulkIndex::verifyCanCreateShardKeyIndex(
    const NamespaceString& nss, std::string* errMsg) const {}

void ValidationBehaviorsReshardingBulkIndex::createShardKeyIndex(
    const NamespaceString& nss,
    const BSONObj& proposedKey,
    const boost::optional<BSONObj>& defaultCollation,
    bool unique,
    boost::optional<TimeseriesOptions> tsOpts) const {
    tassert(7711203,
            "Resharding isn't supported on time-series collection, so 'tsOpts' should never hold a "
            "value",
            !tsOpts);

    BSONObj collation =
        defaultCollation && !defaultCollation->isEmpty() ? CollationSpec::kSimpleSpec : BSONObj();
    _shardKeyIndexSpec = makeIndexSpec(nss, proposedKey, collation, unique, tsOpts);
}

void ValidationBehaviorsReshardingBulkIndex::setOpCtxAndCloneTimestamp(OperationContext* opCtx,
                                                                       Timestamp cloneTimestamp) {
    _opCtx = opCtx;
    _cloneTimestamp = cloneTimestamp;
}

boost::optional<BSONObj> ValidationBehaviorsReshardingBulkIndex::getShardKeyIndexSpec() const {
    return _shardKeyIndexSpec;
}

}  // namespace shardkeyutil
}  // namespace mongo
