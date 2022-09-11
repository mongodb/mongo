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

#include "mongo/db/catalog/coll_mod.h"

#include <boost/optional.hpp>

#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/coll_mod_index.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterDatabaseLock);
MONGO_FAIL_POINT_DEFINE(hangAfterCollModIndexUniqueFullIndexScan);
MONGO_FAIL_POINT_DEFINE(hangAfterCollModIndexUniqueReleaseIXLock);

void assertNoMovePrimaryInProgress(OperationContext* opCtx, NamespaceString const& nss) {
    try {
        auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquire(
            opCtx, nss.dbName(), DSSAcquisitionMode::kShared);

        auto css = CollectionShardingState::get(opCtx, nss);
        auto collDesc = css->getCollectionDescription(opCtx);
        collDesc.throwIfReshardingInProgress(nss);

        if (!collDesc.isSharded()) {
            if (scopedDss->isMovePrimaryInProgress()) {
                LOGV2(4945200, "assertNoMovePrimaryInProgress", "namespace"_attr = nss.toString());

                uasserted(ErrorCodes::MovePrimaryInProgress,
                          "movePrimary is in progress for namespace " + nss.toString());
            }
        }
    } catch (const DBException& ex) {
        if (ex.toStatus() != ErrorCodes::MovePrimaryInProgress) {
            LOGV2(4945201, "Error when getting collection description", "what"_attr = ex.what());
            return;
        }
        throw;
    }
}

struct ParsedCollModRequest {
    ParsedCollModIndexRequest indexRequest;
    std::string viewOn = {};
    boost::optional<Collection::Validator> collValidator;
    boost::optional<ValidationActionEnum> collValidationAction;
    boost::optional<ValidationLevelEnum> collValidationLevel;
    bool recordPreImages = false;
    boost::optional<ChangeStreamPreAndPostImagesOptions> changeStreamPreAndPostImagesOptions;
    int numModifications = 0;
    bool dryRun = false;
    boost::optional<long long> cappedSize;
    boost::optional<long long> cappedMax;
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

StatusWith<std::pair<ParsedCollModRequest, BSONObj>> parseCollModRequest(OperationContext* opCtx,
                                                                         const NamespaceString& nss,
                                                                         const CollectionPtr& coll,
                                                                         const CollMod& cmd) {

    bool isView = !coll;
    bool isTimeseries = coll && coll->getTimeseriesOptions() != boost::none;

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

        if ((cmdIndex.getUnique() || cmdIndex.getPrepareUnique()) &&
            !feature_flags::gCollModIndexUnique.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            return {ErrorCodes::InvalidOptions,
                    "collMod does not support converting an index to 'unique' or to "
                    "'prepareUnique' mode"};
        }

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
            if (coll->isCapped()) {
                return {ErrorCodes::InvalidOptions,
                        "TTL indexes are not supported for capped collections."};
            }
            if (auto status = index_key_validate::validateExpireAfterSeconds(
                    *cmdIndex.getExpireAfterSeconds(),
                    index_key_validate::ValidateExpireAfterSecondsMode::kSecondaryTTLIndex);
                !status.isOK()) {
                return {ErrorCodes::InvalidOptions, status.reason()};
            }
        }

        if (cmdIndex.getHidden() && coll->isClustered() && !nss.isTimeseriesBucketsCollection()) {
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
                        str::stream() << "cannot find index " << indexName << " for ns " << nss};
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
                        str::stream() << "cannot find index " << keyPattern << " for ns " << nss};
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
                // Disallow one-step unique convertion. The user has to set
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
            if (nss.isSystem() && !nss.isTimeseriesBucketsCollection()) {
                return {ErrorCodes::BadValue, "Can't hide index on system collection"};
            }

            // Disallow index hiding/unhiding on _id indexes - these are created by default and
            // are critical to most collection operations.
            if (cmrIndex->idx->isIdIndex()) {
                return {ErrorCodes::BadValue, "can't hide _id index"};
            }

            // Hiding a hidden index or unhiding a visible index should be treated as a no-op.
            if (cmrIndex->idx->hidden() == *cmdIndex.getHidden()) {
                indexForOplog->setHidden(boost::none);
            } else {
                cmrIndex->indexHidden = cmdIndex.getHidden();
            }
        }

        if (cmdIndex.getPrepareUnique()) {
            parsed.numModifications++;
            // Attempting to modify with the same value should be treated as a no-op.
            if (cmrIndex->idx->prepareUnique() == *cmdIndex.getPrepareUnique() ||
                cmrIndex->idx->unique()) {
                indexForOplog->setPrepareUnique(boost::none);
            } else {
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
        // If the feature compatibility version is not kLatest, and we are validating features as
        // primary, ban the use of new agg features introduced in kLatest to prevent them from being
        // persisted in the catalog.
        boost::optional<multiversion::FeatureCompatibilityVersion> maxFeatureCompatibilityVersion;
        // (Generic FCV reference): This FCV check should exist across LTS binary versions.
        multiversion::FeatureCompatibilityVersion fcv;
        if (serverGlobalParams.validateFeaturesAsPrimary.load() &&
            serverGlobalParams.featureCompatibility.isLessThan(multiversion::GenericFCV::kLatest,
                                                               &fcv)) {
            maxFeatureCompatibilityVersion = fcv;
        }
        auto validatorObj = *validator;
        parsed.collValidator = coll->parseValidator(opCtx,
                                                    validatorObj.getOwned(),
                                                    MatchExpressionParser::kDefaultSpecialFeatures,
                                                    maxFeatureCompatibilityVersion);
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
        parsed.viewOn = viewOn->toString();
        oplogEntryBuilder.append(CollMod::kViewOnFieldName, *viewOn);
    }

    if (const auto& recordPreImages = cmr.getRecordPreImages()) {
        if (isView) {
            return getNotSupportedOnViewError(CollMod::kRecordPreImagesFieldName);
        }
        if (isTimeseries) {
            return getNotSupportedOnTimeseriesError(CollMod::kRecordPreImagesFieldName);
        }
        parsed.numModifications++;
        parsed.recordPreImages = *recordPreImages;
        oplogEntryBuilder.append(CollMod::kRecordPreImagesFieldName, *recordPreImages);
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

        auto status = stdx::visit(
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
    const stdx::variant<std::string, std::int64_t>& clusteredIndexExpireAfterSeconds) {
    invariant(oldCollOptions.clusteredIndex);

    boost::optional<int64_t> oldExpireAfterSeconds = oldCollOptions.expireAfterSeconds;

    stdx::visit(
        OverloadedVisitor{
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

                // If this collection was not previously TTL, inform the TTL monitor when we commit.
                if (!oldExpireAfterSeconds) {
                    auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
                    opCtx->recoveryUnit()->onCommit([ttlCache, uuid = coll->uuid()](auto _) {
                        ttlCache->registerTTLInfo(
                            uuid, TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}});
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
    const auto& collection = coll.getCollection();

    // Validate collMod request and look up index descriptor for checking duplicates.
    auto statusW = parseCollModRequest(opCtx, nss, collection, cmd);
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
    auto violatingRecordsList = scanIndexForDuplicates(opCtx, collection, cmr.indexRequest.idx);
    if (!violatingRecordsList.empty()) {
        uassertStatusOK(buildConvertUniqueErrorStatus(opCtx, collection, violatingRecordsList));
    }

    return Status::OK();
}

StatusWith<const IndexDescriptor*> _setUpCollModIndexUnique(OperationContext* opCtx,
                                                            const NamespaceStringOrUUID& nsOrUUID,
                                                            const CollMod& cmd) {
    // Acquires the MODE_IX lock with the intent to write to the collection later in the collMod
    // operation while still allowing concurrent writes. This also makes sure the operation is
    // killed during a stepdown.
    AutoGetCollection coll(opCtx, nsOrUUID, MODE_IX);
    auto nss = coll.getNss();

    const auto& collection = coll.getCollection();
    if (!collection) {
        checkCollectionUUIDMismatch(opCtx, nss, nullptr, cmd.getCollectionUUID());
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "ns does not exist for unique index conversion: " << nss);
    }

    // Scan index for duplicates without exclusive access.
    auto statusW = parseCollModRequest(opCtx, nss, collection, cmd);
    if (!statusW.isOK()) {
        return statusW.getStatus();
    }
    const auto& cmr = statusW.getValue().first;
    auto idx = cmr.indexRequest.idx;
    auto violatingRecordsList = scanIndexForDuplicates(opCtx, collection, idx);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(&hangAfterCollModIndexUniqueFullIndexScan,
                                                     opCtx,
                                                     "hangAfterCollModIndexUniqueFullIndexScan",
                                                     []() {},
                                                     nss);

    if (!violatingRecordsList.empty()) {
        uassertStatusOK(buildConvertUniqueErrorStatus(opCtx, collection, violatingRecordsList));
    }

    return idx;
}

Status _collModInternal(OperationContext* opCtx,
                        const NamespaceStringOrUUID& nsOrUUID,
                        const CollMod& cmd,
                        BSONObjBuilder* result,
                        boost::optional<repl::OplogApplication::Mode> mode) {
    if (cmd.getDryRun().value_or(false)) {
        return _processCollModDryRunMode(opCtx, nsOrUUID, cmd, result, mode);
    }

    // Before acquiring exclusive access to the collection for unique index conversion, we will
    // perform a preliminary index scan here. After we obtain exclusive access for the actual
    // conversion, we will check if the index has ever been modified since the scan before updating
    // the catalog.
    const IndexDescriptor* idx_first = nullptr;
    if (cmd.getIndex() && cmd.getIndex()->getUnique().value_or(false) && !mode) {
        auto statusW = _setUpCollModIndexUnique(opCtx, nsOrUUID, cmd);
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

    AutoGetCollection coll(opCtx, nsOrUUID, MODE_X, AutoGetCollectionViewMode::kViewsPermitted);
    auto nss = coll.getNss();
    StringData dbName = nss.db();
    Lock::CollectionLock systemViewsLock(
        opCtx, NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName), MODE_X);

    Database* const db = coll.getDb();

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
        assertNoMovePrimaryInProgress(opCtx, nss);
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(coll->uuid());
    }

    // If db/collection/view does not exist, short circuit and return.
    if (!db || (!coll && !view)) {
        if (nss.isTimeseriesBucketsCollection()) {
            // If a sharded time-series collection is dropped, it's possible that a stale mongos
            // sends the request on the buckets namespace instead of the view namespace. Ensure that
            // the shardVersion is upto date before throwing an error.
            CollectionShardingState::get(opCtx, nss)->checkShardVersionOrThrow(opCtx);
        }
        checkCollectionUUIDMismatch(opCtx, nss, nullptr, cmd.getCollectionUUID());
        return Status(ErrorCodes::NamespaceNotFound, "ns does not exist");
    }

    // This is necessary to set up CurOp, update the Top stats, and check shard version if the
    // operation is not on a view.
    OldClientContext ctx(opCtx, nss, !view);

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while setting collection options on " << nss);
    }

    auto statusW = parseCollModRequest(opCtx, nss, coll.getCollection(), cmd);
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
        LOGV2(5324200, "CMD: collMod", "cmdObj"_attr = cmd.toBSON(BSONObj()));
    }

    return writeConflictRetry(opCtx, "collMod", nss.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);

        // Handle collMod on a view and return early. The CollectionCatalog handles the creation of
        // oplog entries for modifications on a view.
        if (view) {
            if (cmd.getPipeline())
                view->setPipeline(*cmd.getPipeline());

            if (!viewOn.empty())
                view->setViewOn(NamespaceString(dbName, viewOn));

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

        // If 'changeStreamPreAndPostImagesOptions' are enabled, 'recordPreImages' must be set
        // to false. If 'recordPreImages' is set to true, 'changeStreamPreAndPostImagesOptions'
        // must be disabled.
        if (cmrNew.changeStreamPreAndPostImagesOptions &&
            cmrNew.changeStreamPreAndPostImagesOptions->getEnabled()) {
            cmrNew.recordPreImages = false;
        }

        if (cmrNew.recordPreImages) {
            cmrNew.changeStreamPreAndPostImagesOptions = ChangeStreamPreAndPostImagesOptions(false);
        }
        if (cmrNew.cappedSize || cmrNew.cappedMax) {
            // If the current capped collection size exceeds the newly set limits, future document
            // inserts will prompt document deletion.
            uassertStatusOK(coll.getWritableCollection(opCtx)->updateCappedSize(
                opCtx, cmrNew.cappedSize, cmrNew.cappedMax));
        }

        boost::optional<IndexCollModInfo> indexCollModInfo;

        // Handle collMod operation type appropriately.
        if (cmd.getExpireAfterSeconds()) {
            _setClusteredExpireAfterSeconds(opCtx,
                                            oldCollOptions,
                                            coll.getWritableCollection(opCtx),
                                            *cmd.getExpireAfterSeconds());
        }

        // Handle index modifications.
        processCollModIndexRequest(
            opCtx, &coll, cmrNew.indexRequest, &indexCollModInfo, result, mode);

        if (cmrNew.collValidator) {
            coll.getWritableCollection(opCtx)->setValidator(opCtx, *cmrNew.collValidator);
        }
        if (cmrNew.collValidationAction)
            uassertStatusOKWithContext(coll.getWritableCollection(opCtx)->setValidationAction(
                                           opCtx, *cmrNew.collValidationAction),
                                       "Failed to set validationAction");
        if (cmrNew.collValidationLevel) {
            uassertStatusOKWithContext(coll.getWritableCollection(opCtx)->setValidationLevel(
                                           opCtx, *cmrNew.collValidationLevel),
                                       "Failed to set validationLevel");
        }

        if (cmrNew.recordPreImages != oldCollOptions.recordPreImages) {
            coll.getWritableCollection(opCtx)->setRecordPreImages(opCtx, cmrNew.recordPreImages);
        }

        if (cmrNew.changeStreamPreAndPostImagesOptions.has_value() &&
            *cmrNew.changeStreamPreAndPostImagesOptions !=
                oldCollOptions.changeStreamPreAndPostImagesOptions) {
            coll.getWritableCollection(opCtx)->setChangeStreamPreAndPostImages(
                opCtx, *cmrNew.changeStreamPreAndPostImagesOptions);
        }

        if (ts) {
            auto res =
                timeseries::applyTimeseriesOptionsModifications(*oldCollOptions.timeseries, *ts);
            uassertStatusOK(res);
            auto [newOptions, changed] = res.getValue();
            if (changed) {
                coll.getWritableCollection(opCtx)->setTimeseriesOptions(opCtx, newOptions);
            }
        }

        // Fix any invalid index options for indexes belonging to this collection.
        std::vector<std::string> indexesWithInvalidOptions =
            coll.getWritableCollection(opCtx)->repairInvalidIndexOptions(opCtx);
        for (const auto& indexWithInvalidOptions : indexesWithInvalidOptions) {
            const IndexDescriptor* desc =
                coll->getIndexCatalog()->findIndexByName(opCtx, indexWithInvalidOptions);
            invariant(desc);

            // Notify the index catalog that the definition of this index changed.
            coll.getWritableCollection(opCtx)->getIndexCatalog()->refreshEntry(
                opCtx, coll.getWritableCollection(opCtx), desc, CreateIndexEntryFlags::kIsReady);
        }

        // Only observe non-view collMods, as view operations are observed as operations on the
        // system.views collection.
        auto* const opObserver = opCtx->getServiceContext()->getOpObserver();
        opObserver->onCollMod(
            opCtx, nss, coll->uuid(), oplogEntryObj, oldCollOptions, indexCollModInfo);

        wunit.commit();
        return Status::OK();
    });
}

}  // namespace

Status processCollModCommand(OperationContext* opCtx,
                             const NamespaceStringOrUUID& nsOrUUID,
                             const CollMod& cmd,
                             BSONObjBuilder* result) {
    return _collModInternal(opCtx, nsOrUUID, cmd, result, boost::none);
}

Status processCollModCommandForApplyOps(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        const CollMod& cmd,
                                        repl::OplogApplication::Mode mode) {
    BSONObjBuilder resultWeDontCareAbout;
    return _collModInternal(opCtx, nsOrUUID, cmd, &resultWeDontCareAbout, mode);
}

}  // namespace mongo
