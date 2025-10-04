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

#include "mongo/db/local_catalog/coll_mod.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/coll_mod_index.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_options_gen.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/ttl/ttl_collection_cache.h"
#include "mongo/db/version_context.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterDatabaseLock);
MONGO_FAIL_POINT_DEFINE(hangAfterCollModIndexUniqueFullIndexScan);
MONGO_FAIL_POINT_DEFINE(hangAfterCollModIndexUniqueReleaseIXLock);

struct ParsedCollModRequest {
    ParsedCollModIndexRequest indexRequest;
    std::string viewOn = {};
    boost::optional<Collection::Validator> collValidator;
    boost::optional<ValidationActionEnum> collValidationAction;
    boost::optional<ValidationLevelEnum> collValidationLevel;
    boost::optional<ChangeStreamPreAndPostImagesOptions> changeStreamPreAndPostImagesOptions;
    int numModifications = 0;
    bool dryRun = false;
    boost::optional<long long> cappedSize;
    boost::optional<long long> cappedMax;
    boost::optional<bool> timeseriesBucketsMayHaveMixedSchemaData;
    // TODO(SERVER-101423): Remove once 9.0 becomes last LTS.
    boost::optional<bool> _removeLegacyTimeseriesBucketingParametersHaveChanged;
    boost::optional<bool> recordIdsReplicated;
};

Status getNotSupportedOnViewError(StringData fieldName) {
    return {ErrorCodes::InvalidOptions,
            str::stream() << "option not supported on a view: " << fieldName};
}

Status getOnlySupportedOnViewError(StringData fieldName) {
    return {ErrorCodes::InvalidOptions,
            str::stream() << "option only supported on a view: " << fieldName};
}

Status getNotSupportedOnTimeseriesError(StringData fieldName) {
    return {ErrorCodes::InvalidOptions,
            str::stream() << "option not supported on a time-series collection: " << fieldName};
}

Status getOnlySupportedOnTimeseriesError(StringData fieldName) {
    return {ErrorCodes::InvalidOptions,
            str::stream() << "option only supported on a time-series collection: " << fieldName};
}

boost::optional<ShardKeyPattern> getShardKeyPatternIfSharded(OperationContext* opCtx,
                                                             const NamespaceStringOrUUID& nsOrUUID,
                                                             const CollMod& cmd) {
    if (!Grid::get(opCtx)->isInitialized()) {
        return boost::none;
    }

    try {
        const NamespaceString nss =
            CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
        if (auto catalogClient = Grid::get(opCtx)->catalogClient()) {
            auto coll = catalogClient->getCollection(opCtx, nss);
            if (coll.getUnsplittable()) {
                return boost::none;
            } else {
                return ShardKeyPattern(coll.getKeyPattern());
            }
        }
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is unsharded or doesn't exist.
    }

    return boost::none;
}

StatusWith<std::pair<ParsedCollModRequest, BSONObj>> parseCollModRequest(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionPtr& coll,
    const CollMod& cmd,
    const boost::optional<ShardKeyPattern>& shardKeyPattern) {

    bool isView = !coll;
    bool isTimeseries = coll && coll->isTimeseriesCollection();

    ParsedCollModRequest parsed;
    auto& cmr = cmd.getCollModRequest();
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append(CollMod::kCommandName, cmd.getNamespace().coll());

    try {
        checkCollectionUUIDMismatch(opCtx, nss, coll, cmr.getCollectionUUID());
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (cmr.getCappedSize() || cmr.getCappedMax()) {
        if (!coll->isCapped()) {
            return {ErrorCodes::InvalidOptions, "Collection must be capped."};
        } else if (coll->ns().isOplog()) {
            return {ErrorCodes::InvalidOptions,
                    "Cannot resize the oplog using this command. Use the "
                    "'replSetResizeOplog' command instead."};
        } else {
            parsed.cappedSize = cmr.getCappedSize();
            parsed.cappedMax = cmr.getCappedMax();
        }

        if (const auto& cappedSize = cmr.getCappedSize()) {
            auto swCappedSize = CollectionOptions::checkAndAdjustCappedSize(*cappedSize);
            if (!swCappedSize.isOK()) {
                return swCappedSize.getStatus();
            }
            parsed.cappedSize = swCappedSize.getValue();
            oplogEntryBuilder.append(CollMod::kCappedSizeFieldName, *cappedSize);
        }
        if (const auto& cappedMax = cmr.getCappedMax()) {
            auto swCappedMaxDocs = CollectionOptions::checkAndAdjustCappedMaxDocs(*cappedMax);
            if (!swCappedMaxDocs.isOK()) {
                return swCappedMaxDocs.getStatus();
            }
            parsed.cappedMax = swCappedMaxDocs.getValue();
            oplogEntryBuilder.append(CollMod::kCappedMaxFieldName, *cappedMax);
        }
    }

    if (auto& index = cmr.getIndex()) {
        if (isView) {
            return getNotSupportedOnViewError(CollMod::kIndexFieldName);
        }
        const auto& cmdIndex = *index;
        StringData indexName;
        BSONObj keyPattern;

        if (cmdIndex.getName() && cmdIndex.getKeyPattern()) {
            return {ErrorCodes::InvalidOptions, "Cannot specify both key pattern and name."};
        }

        if (!cmdIndex.getName() && !cmdIndex.getKeyPattern()) {
            return {ErrorCodes::InvalidOptions, "Must specify either index name or key pattern."};
        }

        if (cmdIndex.getName()) {
            indexName = *cmdIndex.getName();
        }

        if (cmdIndex.getKeyPattern()) {
            keyPattern = *cmdIndex.getKeyPattern();
        }

        if (!cmdIndex.getExpireAfterSeconds() && !cmdIndex.getHidden() && !cmdIndex.getUnique() &&
            !cmdIndex.getPrepareUnique() && !cmdIndex.getForceNonUnique()) {
            return {ErrorCodes::InvalidOptions,
                    "no expireAfterSeconds, hidden, unique, or prepareUnique field"};
        }

        auto cmrIndex = &parsed.indexRequest;
        if (cmdIndex.getUnique() && cmdIndex.getForceNonUnique()) {
            return {ErrorCodes::InvalidOptions,
                    "collMod does not support 'unique' and 'forceNonUnique' options at the "
                    "same time"};
        }

        if (cmdIndex.getExpireAfterSeconds()) {
            if (isTimeseries) {
                return {ErrorCodes::InvalidOptions,
                        "TTL indexes are not supported for time-series collections. "
                        "Please refer to the documentation and use the top-level "
                        "'expireAfterSeconds' option instead"};
            }
            if (auto status = index_key_validate::validateExpireAfterSeconds(
                    *cmdIndex.getExpireAfterSeconds(),
                    index_key_validate::ValidateExpireAfterSecondsMode::kSecondaryTTLIndex);
                !status.isOK()) {
                return {ErrorCodes::InvalidOptions, status.reason()};
            }
        }

        if (cmdIndex.getHidden() && coll->isClustered() && !coll->isTimeseriesCollection()) {
            auto clusteredInfo = coll->getClusteredInfo();
            tassert(6011801,
                    "Collection isClustered() and getClusteredInfo() should be synced",
                    clusteredInfo);

            const auto& indexSpec = clusteredInfo->getIndexSpec();

            tassert(6011802,
                    "When not provided by the user, a default name should always be generated "
                    "for the collection's clusteredIndex",
                    indexSpec.getName());

            if ((!indexName.empty() && indexName == StringData(indexSpec.getName().value())) ||
                keyPattern.woCompare(indexSpec.getKey()) == 0) {
                // The indexName or keyPattern match the collection's clusteredIndex.
                return {ErrorCodes::Error(6011800),
                        "The 'hidden' option is not supported for a clusteredIndex"};
            }
        }
        if (!indexName.empty()) {
            cmrIndex->idx = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
            if (!cmrIndex->idx) {
                return {ErrorCodes::IndexNotFound,
                        str::stream() << "cannot find index " << indexName << " for ns "
                                      << nss.toStringForErrorMsg()};
            }
        } else {
            std::vector<const IndexDescriptor*> indexes;
            coll->getIndexCatalog()->findIndexesByKeyPattern(
                opCtx, keyPattern, IndexCatalog::InclusionPolicy::kReady, &indexes);

            if (indexes.size() > 1) {
                return {ErrorCodes::AmbiguousIndexKeyPattern,
                        str::stream() << "index keyPattern " << keyPattern << " matches "
                                      << indexes.size() << " indexes,"
                                      << " must use index name. "
                                      << "Conflicting indexes:" << indexes[0]->infoObj() << ", "
                                      << indexes[1]->infoObj()};
            } else if (indexes.empty()) {
                return {ErrorCodes::IndexNotFound,
                        str::stream() << "cannot find index " << keyPattern << " for ns "
                                      << nss.toStringForErrorMsg()};
            }

            cmrIndex->idx = indexes[0];
        }

        if (cmdIndex.getExpireAfterSeconds()) {
            parsed.numModifications++;
            BSONElement oldExpireSecs = cmrIndex->idx->infoObj().getField("expireAfterSeconds");
            if (oldExpireSecs.eoo()) {
                if (cmrIndex->idx->isIdIndex()) {
                    return {ErrorCodes::InvalidOptions,
                            "the _id field does not support TTL indexes"};
                }
                if (cmrIndex->idx->getNumFields() != 1) {
                    return {ErrorCodes::InvalidOptions,
                            "TTL indexes are single-field indexes, compound indexes do "
                            "not support TTL"};
                }
            } else if (!oldExpireSecs.isNumber()) {
                return {ErrorCodes::InvalidOptions,
                        "existing expireAfterSeconds field is not a number"};
            }

            cmrIndex->indexExpireAfterSeconds = *cmdIndex.getExpireAfterSeconds();
        }

        // Make a copy of the index options doc for writing to the oplog.
        // This index options doc should exclude the options that do not need
        // to be modified.
        auto indexForOplog = index;

        if (cmdIndex.getUnique()) {
            parsed.numModifications++;
            if (bool unique = *cmdIndex.getUnique(); !unique) {
                return {ErrorCodes::BadValue, "Cannot make index non-unique"};
            }

            // Attempting to converting a unique index should be treated as a no-op.
            if (cmrIndex->idx->unique()) {
                indexForOplog->setUnique(boost::none);
            } else {
                // Disallow one-step unique conversion. The user has to set
                // 'prepareUnique' to true first.
                if (!cmrIndex->idx->prepareUnique()) {
                    return Status(ErrorCodes::InvalidOptions,
                                  "Cannot make index unique with 'prepareUnique=false'. "
                                  "Run collMod to set it first.");
                }
                cmrIndex->indexUnique = true;
            }
        }

        if (cmdIndex.getHidden()) {
            parsed.numModifications++;
            // Disallow index hiding/unhiding on system collections.
            // Bucket collections, which hold data for user-created time-series collections, do
            // not have this restriction.
            //
            // TODO SERVER-105548 remove the check on buckets collection after 9.0 becomes last LTS
            if (nss.isSystem() && !nss.isTimeseriesBucketsCollection()) {
                return {ErrorCodes::BadValue, "Can't hide index on system collection"};
            }

            // Disallow index hiding/unhiding on _id indexes - these are created by default and
            // are critical to most collection operations.
            if (cmrIndex->idx->isIdIndex()) {
                return {ErrorCodes::BadValue, "can't hide _id index"};
            }

            // If the index is not hidden and we are trying to hide it, check if it is possible
            // to drop the shard key index, so it could be possible to hide it.
            if (!cmrIndex->idx->hidden() && *cmdIndex.getHidden()) {
                if (shardKeyPattern) {
                    if (isLastNonHiddenRangedShardKeyIndex(
                            opCtx, coll, cmrIndex->idx->indexName(), shardKeyPattern->toBSON())) {
                        return {ErrorCodes::InvalidOptions,
                                "Can't hide the only compatible index for this collection's "
                                "shard key"};
                    }
                }
            }

            // Hiding a hidden index or unhiding a visible index should be treated as a no-op.
            if (cmrIndex->idx->hidden() == *cmdIndex.getHidden()) {
                indexForOplog->setHidden(boost::none);
            } else {
                cmrIndex->indexHidden = cmdIndex.getHidden();
            }
        }

        if (cmdIndex.getPrepareUnique()) {
            // Check if prepareUnique is being set on a time-series collection index.
            if (isTimeseries) {
                return {ErrorCodes::InvalidOptions,
                        "cannot set 'prepareUnique' for indexes of a time-series collection."};
            }
            parsed.numModifications++;
            // Attempting to modify with the same value should be treated as a no-op.
            if (cmrIndex->idx->prepareUnique() == *cmdIndex.getPrepareUnique() ||
                cmrIndex->idx->unique()) {
                indexForOplog->setPrepareUnique(boost::none);
            } else {
                // Checks if the index key pattern conflicts with the shard key pattern.
                if (shardKeyPattern) {
                    if (!shardKeyPattern->isIndexUniquenessCompatible(
                            cmrIndex->idx->keyPattern())) {
                        return {
                            ErrorCodes::InvalidOptions,
                            fmt::format("cannot set 'prepareUnique' for index {} with shard key "
                                        "pattern {}",
                                        cmrIndex->idx->keyPattern().toString(),
                                        shardKeyPattern->toBSON().toString())};
                    }
                }
                cmrIndex->indexPrepareUnique = cmdIndex.getPrepareUnique();
            }
        }

        if (cmdIndex.getForceNonUnique()) {
            parsed.numModifications++;
            if (bool unique = *cmdIndex.getForceNonUnique(); !unique) {
                return {ErrorCodes::BadValue, "'forceNonUnique: false' is not supported"};
            }

            // Attempting to convert a non-unique index should be treated as a no-op.
            if (!cmrIndex->idx->unique()) {
                indexForOplog->setForceNonUnique(boost::none);
            } else {
                cmrIndex->indexForceNonUnique = true;
            }
        }
        // The index options doc must contain either the name or key pattern, but not both.
        // If we have just one field, the index modifications requested matches the current
        // state in catalog and there is nothing further to do.
        BSONObjBuilder indexForOplogObjBuilder;
        indexForOplog->serialize(&indexForOplogObjBuilder);
        auto indexForOplogObj = indexForOplogObjBuilder.obj();
        if (indexForOplogObj.nFields() > 1) {
            oplogEntryBuilder.append(CollMod::kIndexFieldName, indexForOplogObj);
        }
    }

    if (auto& validator = cmr.getValidator()) {
        if (isView) {
            return getNotSupportedOnViewError(CollMod::kValidatorFieldName);
        }
        if (isTimeseries) {
            return getNotSupportedOnTimeseriesError(CollMod::kValidatorFieldName);
        }
        parsed.numModifications++;
        auto validatorObj = *validator;
        parsed.collValidator = coll->parseValidator(
            opCtx, validatorObj.getOwned(), MatchExpressionParser::kDefaultSpecialFeatures);

        // Increment counters to track the usage of schema validators.
        validatorCounters.incrementCounters(
            cmd.kCommandName, parsed.collValidator->validatorDoc, parsed.collValidator->isOK());

        if (!parsed.collValidator->isOK()) {
            return parsed.collValidator->getStatus();
        }
        oplogEntryBuilder.append(CollMod::kValidatorFieldName, validatorObj);
    }

    if (const auto& validationLevel = cmr.getValidationLevel()) {
        if (isView) {
            return getNotSupportedOnViewError(CollMod::kValidationLevelFieldName);
        }
        if (isTimeseries) {
            return getNotSupportedOnTimeseriesError(CollMod::kValidationLevelFieldName);
        }
        parsed.numModifications++;
        parsed.collValidationLevel = *validationLevel;
        oplogEntryBuilder.append(CollMod::kValidationLevelFieldName,
                                 ValidationLevel_serializer(*validationLevel));
    }

    if (const auto& validationAction = cmr.getValidationAction()) {
        if (isView) {
            return getNotSupportedOnViewError(CollMod::kValidationActionFieldName);
        }
        if (isTimeseries) {
            return getNotSupportedOnTimeseriesError(CollMod::kValidationActionFieldName);
        }
        parsed.numModifications++;
        parsed.collValidationAction = *validationAction;
        oplogEntryBuilder.append(CollMod::kValidationActionFieldName,
                                 ValidationAction_serializer(*validationAction));
    }

    if (auto& pipeline = cmr.getPipeline()) {
        parsed.numModifications++;
        if (!isView) {
            return getOnlySupportedOnViewError(CollMod::kPipelineFieldName);
        }
        oplogEntryBuilder.append(CollMod::kPipelineFieldName, *pipeline);
    }

    if (const auto& viewOn = cmr.getViewOn()) {
        parsed.numModifications++;
        if (!isView) {
            return getOnlySupportedOnViewError(CollMod::kViewOnFieldName);
        }
        parsed.viewOn = std::string{*viewOn};
        oplogEntryBuilder.append(CollMod::kViewOnFieldName, *viewOn);
    }

    if (auto& changeStreamPreAndPostImages = cmr.getChangeStreamPreAndPostImages()) {
        if (isView) {
            return getNotSupportedOnViewError(CollMod::kChangeStreamPreAndPostImagesFieldName);
        }
        if (isTimeseries) {
            return getNotSupportedOnTimeseriesError(
                CollMod::kChangeStreamPreAndPostImagesFieldName);
        }
        parsed.numModifications++;
        parsed.changeStreamPreAndPostImagesOptions = *changeStreamPreAndPostImages;

        BSONObjBuilder subObjBuilder(
            oplogEntryBuilder.subobjStart(CollMod::kChangeStreamPreAndPostImagesFieldName));
        changeStreamPreAndPostImages->serialize(&subObjBuilder);
    }

    if (auto& expireAfterSeconds = cmr.getExpireAfterSeconds()) {
        if (isView) {
            return getNotSupportedOnViewError(CollMod::kExpireAfterSecondsFieldName);
        }
        parsed.numModifications++;
        if (coll->getRecordStore()->keyFormat() != KeyFormat::String) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "'" << CollMod::kExpireAfterSecondsFieldName
                                  << "' option is only supported on collections clustered by _id"};
        }

        auto status = visit(
            OverloadedVisitor{
                [&oplogEntryBuilder](const std::string& value) -> Status {
                    if (value != "off") {
                        return {ErrorCodes::InvalidOptions,
                                str::stream()
                                    << "Invalid string value for the "
                                       "'clusteredIndex::expireAfterSeconds' "
                                    << "option. Got: '" << value << "'. Accepted value is 'off'"};
                    }
                    oplogEntryBuilder.append(CollMod::kExpireAfterSecondsFieldName, value);
                    return Status::OK();
                },
                [&oplogEntryBuilder](std::int64_t value) {
                    oplogEntryBuilder.append(CollMod::kExpireAfterSecondsFieldName, value);
                    return index_key_validate::validateExpireAfterSeconds(
                        value,
                        index_key_validate::ValidateExpireAfterSecondsMode::kClusteredTTLIndex);
                },
            },
            *expireAfterSeconds);
        if (!status.isOK()) {
            return status;
        }
    }

    if (auto& timeseries = cmr.getTimeseries()) {
        parsed.numModifications++;
        if (!isTimeseries) {
            return getOnlySupportedOnTimeseriesError(CollMod::kTimeseriesFieldName);
        }

        BSONObjBuilder subObjBuilder(oplogEntryBuilder.subobjStart(CollMod::kTimeseriesFieldName));
        timeseries->serialize(&subObjBuilder);
    }

    if (auto mixedSchema = cmr.getTimeseriesBucketsMayHaveMixedSchemaData()) {
        if (!isTimeseries) {
            return getOnlySupportedOnTimeseriesError(
                CollMod::kTimeseriesBucketsMayHaveMixedSchemaDataFieldName);
        }

        parsed.timeseriesBucketsMayHaveMixedSchemaData = mixedSchema;
        oplogEntryBuilder.append(CollMod::kTimeseriesBucketsMayHaveMixedSchemaDataFieldName,
                                 *mixedSchema);
    }

    if (auto removeLegacyTSBucketingParametersHaveChanged =
            cmr.get_removeLegacyTimeseriesBucketingParametersHaveChanged()) {
        tassert(9123100,
                "_removeLegacyTimeseriesBucketingParametersHaveChanged should only be set to true",
                *removeLegacyTSBucketingParametersHaveChanged);

        tassert(
            9123101,
            "_removeLegacyTimeseriesBucketingParametersHaveChanged needs a time-series collection",
            isTimeseries);

        parsed._removeLegacyTimeseriesBucketingParametersHaveChanged =
            removeLegacyTSBucketingParametersHaveChanged;
        oplogEntryBuilder.append(
            CollMod::k_removeLegacyTimeseriesBucketingParametersHaveChangedFieldName,
            *removeLegacyTSBucketingParametersHaveChanged);
    }

    if (auto recordIdsReplicated = cmr.getRecordIdsReplicated()) {
        if (*recordIdsReplicated) {
            return {ErrorCodes::InvalidOptions, "Cannot set recordIdsReplicated to true"};
        }

        parsed.recordIdsReplicated = recordIdsReplicated;
        oplogEntryBuilder.append(CollMod::kRecordIdsReplicatedFieldName, false);
    }

    if (const auto& dryRun = cmr.getDryRun()) {
        parsed.dryRun = *dryRun;
        // The dry run option should never be included in a collMod oplog entry.
    }

    // Currently disallows the use of 'indexPrepareUnique' with other collMod options.
    if (parsed.indexRequest.indexPrepareUnique && parsed.numModifications > 1) {
        return {ErrorCodes::InvalidOptions,
                "prepareUnique cannot be combined with any other modification."};
    }

    return std::make_pair(std::move(parsed), oplogEntryBuilder.obj());
}

void _setClusteredExpireAfterSeconds(
    OperationContext* opCtx,
    const CollectionOptions& oldCollOptions,
    Collection* coll,
    const std::variant<std::string, std::int64_t>& clusteredIndexExpireAfterSeconds) {
    invariant(oldCollOptions.clusteredIndex);

    boost::optional<int64_t> oldExpireAfterSeconds = oldCollOptions.expireAfterSeconds;

    visit(OverloadedVisitor{
              [&](const std::string& newExpireAfterSeconds) {
                  invariant(newExpireAfterSeconds == "off");
                  if (!oldExpireAfterSeconds) {
                      // expireAfterSeconds is already disabled on the clustered index.
                      return;
                  }

                  coll->updateClusteredIndexTTLSetting(opCtx, boost::none);
              },
              [&](std::int64_t newExpireAfterSeconds) {
                  if (oldExpireAfterSeconds && *oldExpireAfterSeconds == newExpireAfterSeconds) {
                      // expireAfterSeconds is already the requested value on the clustered index.
                      return;
                  }

                  // If this collection was not previously TTL, inform the TTL monitor when we
                  // commit.
                  if (!oldExpireAfterSeconds) {
                      auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
                      shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                          [ttlCache, uuid = coll->uuid()](OperationContext*,
                                                          boost::optional<Timestamp>) {
                              ttlCache->registerTTLInfo(
                                  uuid,
                                  TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}});
                          });
                  }

                  invariant(newExpireAfterSeconds >= 0);
                  coll->updateClusteredIndexTTLSetting(opCtx, newExpireAfterSeconds);
              }},
          clusteredIndexExpireAfterSeconds);
}

Status _processCollModDryRunMode(OperationContext* opCtx,
                                 const NamespaceStringOrUUID& nsOrUUID,
                                 const CollMod& cmd,
                                 const boost::optional<ShardKeyPattern>& shardKeyPattern,
                                 BSONObjBuilder* result,
                                 boost::optional<repl::OplogApplication::Mode> mode) {
    // Ensure that the unique option is specified before validation the rest of the request
    // and resolving the index descriptor.
    if (!cmd.getIndex()) {
        return {ErrorCodes::InvalidOptions, "dry run mode requires an valid index modification."};
    }
    if (!cmd.getIndex()->getUnique().value_or(false)) {
        return {ErrorCodes::InvalidOptions,
                "dry run mode requires an index modification with unique: true."};
    }

    // A collMod operation with dry run mode requested is not meant for replicated oplog entries.
    if (mode) {
        return {ErrorCodes::InvalidOptions,
                "dry run mode is not applicable to oplog application or applyOps"};
    }

    // We do not need write access in dry run mode.
    AutoGetCollection coll(opCtx, nsOrUUID, MODE_IS);
    auto nss = coll.getNss();

    // Validate collMod request and look up index descriptor for checking duplicates.
    auto statusW = parseCollModRequest(opCtx, nss, *coll, cmd, shardKeyPattern);

    if (!statusW.isOK()) {
        return statusW.getStatus();
    }
    const auto& cmr = statusW.getValue().first;

    // The unique option should be set according to the checks at the top of this function.
    // Any other modification requested should lead to us refusing to run collMod in dry run mode.
    if (cmr.numModifications > 1) {
        return {ErrorCodes::InvalidOptions,
                "unique: true cannot be combined with any other modification in dry run mode."};
    }

    // Throws exception if index contains duplicates.
    auto violatingRecords = scanIndexForDuplicates(opCtx, cmr.indexRequest.idx);
    if (!violatingRecords.empty()) {
        uassertStatusOK(buildConvertUniqueErrorStatus(opCtx, (*coll).get(), violatingRecords));
    }

    return Status::OK();
}

StatusWith<const IndexDescriptor*> _setUpCollModIndexUnique(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    const CollMod& cmd,
    const boost::optional<ShardKeyPattern>& shardKeyPattern) {
    // Acquires the MODE_IX lock with the intent to write to the collection later in the collMod
    // operation while still allowing concurrent writes. This also makes sure the operation is
    // killed during a stepdown.
    AutoGetCollection coll(opCtx, nsOrUUID, MODE_IX);
    auto nss = coll.getNss();

    const auto& collection = *coll;
    if (!collection) {
        checkCollectionUUIDMismatch(opCtx, nss, CollectionPtr(), cmd.getCollectionUUID());
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "ns does not exist for unique index conversion: "
                                    << nss.toStringForErrorMsg());
    }

    // Scan index for duplicates without exclusive access.
    auto statusW = parseCollModRequest(opCtx, nss, collection, cmd, shardKeyPattern);

    if (!statusW.isOK()) {
        return statusW.getStatus();
    }
    const auto& cmr = statusW.getValue().first;
    auto idx = cmr.indexRequest.idx;
    auto violatingRecords = scanIndexForDuplicates(opCtx, idx);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangAfterCollModIndexUniqueFullIndexScan,
        opCtx,
        "hangAfterCollModIndexUniqueFullIndexScan",
        []() {},
        nss);

    if (!violatingRecords.empty()) {
        uassertStatusOK(buildConvertUniqueErrorStatus(opCtx, collection.get(), violatingRecords));
    }

    return idx;
}

Status _collModInternal(OperationContext* opCtx,
                        const NamespaceStringOrUUID& nsOrUUID,
                        const CollMod& cmd,
                        CollectionAcquisition* acquisition,
                        BSONObjBuilder* result,
                        boost::optional<repl::OplogApplication::Mode> mode) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();

    // Get key pattern from the config server if we may need it for parsing checks if on the primary
    // before taking any locks. The ddl lock will prevent the key pattern from changing.
    boost::optional<ShardKeyPattern> shardKeyPattern;
    bool mayNeedKeyPatternForParsing =
        cmd.getIndex() && (cmd.getIndex()->getHidden() || cmd.getIndex()->getPrepareUnique());
    if (!mode && mayNeedKeyPatternForParsing) {
        shardKeyPattern = getShardKeyPatternIfSharded(opCtx, nsOrUUID, cmd);
    }

    if (cmd.getDryRun().value_or(false)) {
        return _processCollModDryRunMode(opCtx, nsOrUUID, cmd, shardKeyPattern, result, mode);
    }

    // Before acquiring exclusive access to the collection for unique index conversion, we will
    // perform a preliminary index scan here. After we obtain exclusive access for the actual
    // conversion, we will check if the index has ever been modified since the scan before updating
    // the catalog.
    const IndexDescriptor* idx_first = nullptr;
    if (cmd.getIndex() && cmd.getIndex()->getUnique().value_or(false) && !mode) {
        auto statusW = _setUpCollModIndexUnique(opCtx, nsOrUUID, cmd, shardKeyPattern);
        if (!statusW.isOK()) {
            return statusW.getStatus();
        }
        idx_first = statusW.getValue();
    }

    hangAfterCollModIndexUniqueReleaseIXLock.executeIf(
        [](auto&&) { hangAfterCollModIndexUniqueReleaseIXLock.pauseWhileSet(); },
        [&cmd, &mode](auto&&) {
            return cmd.getIndex() && cmd.getIndex()->getUnique().value_or(false) && !mode;
        });

    boost::optional<AutoGetCollection> autoget;
    if (!acquisition) {
        autoget.emplace(opCtx,
                        nsOrUUID,
                        MODE_X,
                        auto_get_collection::Options{}.viewMode(
                            auto_get_collection::ViewMode::kViewsPermitted));
    }
    auto nss = acquisition ? acquisition->nss() : autoget->getNss();
    auto& coll = acquisition ? acquisition->getCollectionPtr() : *autoget.get();
    auto dbName = nss.dbName();
    Lock::CollectionLock systemViewsLock(
        opCtx, NamespaceString::makeSystemDotViewsNamespace(dbName), MODE_X);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangAfterDatabaseLock, opCtx, "hangAfterDatabaseLock", []() {}, nss);

    // May also modify a view instead of a collection.
    boost::optional<ViewDefinition> view;
    if (!coll) {
        const auto sharedView = CollectionCatalog::get(opCtx)->lookupView(opCtx, nss);
        if (sharedView) {
            // We copy the ViewDefinition as it is modified below to represent the requested state.
            view = {*sharedView};
        }
    }

    // This can kill all cursors so don't allow running it while a background operation is in
    // progress.
    if (coll) {
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(coll->uuid());
    }

    // If collection/view does not exist, short circuit and return.
    if (!coll && !view) {
        // TODO SERVER-105548 remove the check on buckets collection after 9.0 becomes last LTS
        if (nss.isTimeseriesBucketsCollection()) {
            // If a sharded time-series collection is dropped, it's possible that a stale mongos
            // sends the request on the buckets namespace instead of the view namespace. Ensure that
            // the shardVersion is upto date before throwing an error.
            CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
                ->checkShardVersionOrThrow(opCtx);
        }
        checkCollectionUUIDMismatch(opCtx, nss, CollectionPtr(), cmd.getCollectionUUID());
        return Status(ErrorCodes::NamespaceNotFound, "ns does not exist");
    }

    // This is necessary to set up CurOp and update the Top stats.
    AutoStatsTracker statsTracker(opCtx,
                                  nss,
                                  Top::LockType::NotLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(nss.dbName()));

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while setting collection options on "
                                    << nss.toStringForErrorMsg());
    }

    auto statusW = parseCollModRequest(opCtx, nss, coll, cmd, shardKeyPattern);
    if (!statusW.isOK()) {
        return statusW.getStatus();
    }

    auto& cmrNew = statusW.getValue().first;
    auto& oplogEntryObj = statusW.getValue().second;
    auto idx_second = cmrNew.indexRequest.idx;
    if (idx_first && idx_first != idx_second) {
        return Status(
            ErrorCodes::CommandFailed,
            "The index was modified by another thread. Please try to rerun the command and make "
            "sure no other threads are modifying the index.");
    }
    auto viewOn = cmrNew.viewOn;
    auto ts = cmd.getTimeseries();

    if (!serverGlobalParams.quiet.load()) {
        LOGV2(5324200, "CMD: collMod", "cmdObj"_attr = cmd.toBSON());
    }

    return writeConflictRetry(opCtx, "collMod", nss, [&] {
        WriteUnitOfWork wunit(opCtx);

        // Handle collMod on a view and return early. The CollectionCatalog handles the creation of
        // oplog entries for modifications on a view.
        if (view) {
            if (cmd.getPipeline())
                view->setPipeline(*cmd.getPipeline());

            if (!viewOn.empty())
                view->setViewOn(NamespaceStringUtil::deserialize(dbName, viewOn));

            BSONArrayBuilder pipeline;
            for (auto& item : view->pipeline()) {
                pipeline.append(item);
            }
            auto errorStatus =
                CollectionCatalog::get(opCtx)->modifyView(opCtx,
                                                          nss,
                                                          view->viewOn(),
                                                          BSONArray(pipeline.obj()),
                                                          view_catalog_helpers::validatePipeline);
            if (!errorStatus.isOK()) {
                return errorStatus;
            }

            wunit.commit();
            return Status::OK();
        }

        // In order to facilitate the replication rollback process, which makes a best effort
        // attempt to "undo" a set of oplog operations, we store a snapshot of the old collection
        // options to provide to the OpObserver. TTL index updates aren't a part of collection
        // options so we save the relevant TTL index data in a separate object.

        const CollectionOptions& oldCollOptions = coll->getCollectionOptions();

        // Writing invalidates the collection pointer until commit. Snapshot the relevant old
        // collections settings needed before committing.
        const auto timeseriesBucketingParametersHaveChanged =
            coll->timeseriesBucketingParametersHaveChanged();

        auto collWriter = [&] {
            if (acquisition) {
                return CollectionWriter{opCtx, acquisition};
            } else {
                return CollectionWriter{opCtx, autoget.get()};
            }
        }();
        Collection* writableColl = collWriter.getWritableCollection(opCtx);

        if (cmrNew.cappedSize || cmrNew.cappedMax) {
            // If the current capped collection size exceeds the newly set limits, future document
            // inserts will prompt document deletion.
            uassertStatusOK(
                writableColl->updateCappedSize(opCtx, cmrNew.cappedSize, cmrNew.cappedMax));
        }

        boost::optional<IndexCollModInfo> indexCollModInfo;

        // Handle collMod operation type appropriately.
        if (cmd.getExpireAfterSeconds()) {
            _setClusteredExpireAfterSeconds(
                opCtx, oldCollOptions, writableColl, *cmd.getExpireAfterSeconds());
        }

        if (auto mixedSchema = cmrNew.timeseriesBucketsMayHaveMixedSchemaData) {
            writableColl->setTimeseriesBucketsMayHaveMixedSchemaData(opCtx, mixedSchema);
        }

        if (cmrNew._removeLegacyTimeseriesBucketingParametersHaveChanged.has_value()) {
            writableColl->removeLegacyTimeseriesBucketingParametersHaveChanged(opCtx);
        }

        if (auto recordIdsReplicated = cmrNew.recordIdsReplicated) {
            // Must be false if present.
            invariant(
                !(*recordIdsReplicated),
                fmt::format(
                    "Unexpected true value for 'recordIdsReplicated' in collMod options for {}: {}",
                    nss.toStringForErrorMsg(),
                    cmd.toBSON().toString()));

            writableColl->unsetRecordIdsReplicated(opCtx);
        }

        // Handle index modifications.
        processCollModIndexRequest(
            opCtx, writableColl, cmrNew.indexRequest, &indexCollModInfo, result, mode);

        if (cmrNew.collValidator) {
            writableColl->setValidator(opCtx, *cmrNew.collValidator);
        }
        if (cmrNew.collValidationAction)
            uassertStatusOKWithContext(
                writableColl->setValidationAction(opCtx, *cmrNew.collValidationAction),
                "Failed to set validationAction");
        if (cmrNew.collValidationLevel) {
            uassertStatusOKWithContext(
                writableColl->setValidationLevel(opCtx, *cmrNew.collValidationLevel),
                "Failed to set validationLevel");
        }

        if (cmrNew.changeStreamPreAndPostImagesOptions.has_value() &&
            *cmrNew.changeStreamPreAndPostImagesOptions !=
                oldCollOptions.changeStreamPreAndPostImagesOptions) {
            writableColl->setChangeStreamPreAndPostImages(
                opCtx, *cmrNew.changeStreamPreAndPostImagesOptions);
        }

        if (ts) {
            auto res =
                timeseries::applyTimeseriesOptionsModifications(*oldCollOptions.timeseries, *ts);
            uassertStatusOK(res);
            auto [newOptions, changed] = res.getValue();
            if (changed) {
                writableColl->setTimeseriesOptions(opCtx, newOptions);
                if (feature_flags::gTSBucketingParametersUnchanged.isEnabled(
                        VersionContext::getDecoration(opCtx), fcvSnapshot)) {
                    writableColl->setTimeseriesBucketingParametersChanged(opCtx, true);
                };
            }
        }

        const auto version = fcvSnapshot.getVersion();
        // We involve an empty collMod command during a setFCV downgrade to clean timeseries
        // bucketing parameters in the catalog. So if the FCV is in downgrading or downgraded stage,
        // remove time-series bucketing parameters flag, as nodes older than 7.1 cannot understand
        // this flag.
        // (Generic FCV reference): This FCV check should exist across LTS binary versions.
        // TODO SERVER-80003 remove special version handling when LTS becomes 8.0.
        if (cmrNew.numModifications == 0 && timeseriesBucketingParametersHaveChanged &&
            version == multiversion::GenericFCV::kDowngradingFromLatestToLastLTS) {
            writableColl->setTimeseriesBucketingParametersChanged(opCtx, boost::none);
        }

        // Fix any invalid index options for indexes belonging to this collection, only for empty
        // collMod requests which are called during setFCV upgrade.
        const auto removeDeprecatedFields = [&]() {
            if (cmrNew.numModifications > 0) {
                return false;
            }

            if (!ServerGlobalParams::FCVSnapshot::isUpgradingOrDowngrading(version)) {
                return false;
            }

            const auto transitionInfo = getTransitionFCVInfo(version);
            return transitionInfo.from < transitionInfo.to;
        }();

        std::vector<std::string> indexesWithInvalidOptions =
            writableColl->repairInvalidIndexOptions(opCtx, removeDeprecatedFields);
        for (const auto& indexWithInvalidOptions : indexesWithInvalidOptions) {
            const IndexDescriptor* desc =
                writableColl->getIndexCatalog()->findIndexByName(opCtx, indexWithInvalidOptions);
            invariant(desc);

            // Notify the index catalog that the definition of this index changed.
            writableColl->getIndexCatalog()->refreshEntry(
                opCtx, writableColl, desc, CreateIndexEntryFlags::kIsReady);
        }

        // Only observe non-view collMods, as view operations are observed as operations on the
        // system.views collection.
        auto* const opObserver = opCtx->getServiceContext()->getOpObserver();
        opObserver->onCollMod(
            opCtx, nss, writableColl->uuid(), oplogEntryObj, oldCollOptions, indexCollModInfo);

        wunit.commit();
        return Status::OK();
    });
}

}  // namespace

void staticValidateCollMod(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const CollModRequest& request) {
    // Targeting the underlying buckets collection directly would make the time-series
    // Collection out of sync with the time-series view document. Additionally, we want to
    // ultimately obscure/hide the underlying buckets collection from the user, so we're
    // disallowing targetting it.
    uassert(ErrorCodes::InvalidNamespace,
            "collMod on a time-series collection's underlying buckets collection is not "
            "supported.",
            !nss.isTimeseriesBucketsCollection());
}

bool isCollModIndexUniqueConversion(const CollModRequest& request) {
    auto index = request.getIndex();
    if (!index) {
        return false;
    }
    if (auto indexUnique = index->getUnique(); !indexUnique) {
        return false;
    }
    // Checks if the request is an actual unique conversion instead of a dry run.
    if (auto dryRun = request.getDryRun(); dryRun && *dryRun) {
        return false;
    }
    return true;
}

CollModRequest makeCollModDryRunRequest(const CollModRequest& request) {
    CollModRequest dryRunRequest;
    CollModIndex dryRunIndex;
    const auto& requestIndex = request.getIndex();
    dryRunIndex.setUnique(true);
    if (auto keyPattern = requestIndex->getKeyPattern()) {
        dryRunIndex.setKeyPattern(keyPattern);
    } else if (auto name = requestIndex->getName()) {
        dryRunIndex.setName(name);
    }
    if (auto uuid = request.getCollectionUUID()) {
        dryRunRequest.setCollectionUUID(uuid);
    }
    dryRunRequest.setIndex(dryRunIndex);
    dryRunRequest.setDryRun(true);
    return dryRunRequest;
}

Status processCollModCommand(OperationContext* opCtx,
                             const NamespaceStringOrUUID& nsOrUUID,
                             const CollMod& cmd,
                             CollectionAcquisition* acquisition,
                             BSONObjBuilder* result) {
    return _collModInternal(opCtx, nsOrUUID, cmd, acquisition, result, boost::none);
}

Status processCollModCommandForApplyOps(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        const CollMod& cmd,
                                        repl::OplogApplication::Mode mode) {
    BSONObjBuilder resultWeDontCareAbout;
    return _collModInternal(opCtx, nsOrUUID, cmd, nullptr, &resultWeDontCareAbout, mode);
}

}  // namespace mongo
