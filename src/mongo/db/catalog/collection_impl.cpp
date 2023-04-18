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

#include "mongo/db/catalog/collection_impl.h"

#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/catalog/catalog_stats.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog_impl.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog/uncommitted_multikey.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/doc_validation_error.h"
#include "mongo/db/matcher/doc_validation_util.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/implicit_validator.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/capped_snapshots.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

// This fail point allows collections to be given malformed validator. A malformed validator
// will not (and cannot) be enforced but it will be persisted.
MONGO_FAIL_POINT_DEFINE(allowSettingMalformedCollectionValidators);

MONGO_FAIL_POINT_DEFINE(skipCappedDeletes);

Status checkValidatorCanBeUsedOnNs(const BSONObj& validator,
                                   const NamespaceString& nss,
                                   const UUID& uuid) {
    if (validator.isEmpty())
        return Status::OK();

    if (nss.isTemporaryReshardingCollection()) {
        // In resharding, if the user's original collection has a validator, then the temporary
        // resharding collection is created with it as well.
        return Status::OK();
    }

    if (nss.isTimeseriesBucketsCollection()) {
        return Status::OK();
    }

    if (nss.isSystem() && !nss.isDropPendingNamespace()) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Document validators not allowed on system collection "
                              << nss.toStringForErrorMsg() << " with UUID " << uuid};
    }

    // Allow schema on config.settings. This is created internally, and user changes to this
    // validator are disallowed in the createCollection and collMod commands.
    if (nss.isOnInternalDb() && nss != NamespaceString::kConfigSettingsNamespace) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Document validators are not allowed on collection "
                              << nss.toStringForErrorMsg() << " with UUID " << uuid << " in the "
                              << nss.dbName().toStringForErrorMsg() << " internal database"};
    }
    return Status::OK();
}

Status checkValidationOptionsCanBeUsed(const CollectionOptions& opts,
                                       boost::optional<ValidationLevelEnum> newLevel,
                                       boost::optional<ValidationActionEnum> newAction) {
    if (!opts.encryptedFieldConfig) {
        return Status::OK();
    }
    if (validationLevelOrDefault(newLevel) != ValidationLevelEnum::strict) {
        return Status(
            ErrorCodes::BadValue,
            "Validation levels other than 'strict' are not allowed on encrypted collections");
    }
    if (validationActionOrDefault(newAction) == ValidationActionEnum::warn) {
        return Status(ErrorCodes::BadValue,
                      "Validation action of 'warn' is not allowed on encrypted collections");
    }
    return Status::OK();
}

Status validateIsNotInDbs(const NamespaceString& ns,
                          const std::vector<DatabaseName>& disallowedDbs,
                          StringData optionName) {
    if (std::find(disallowedDbs.begin(), disallowedDbs.end(), ns.dbName()) != disallowedDbs.end()) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << optionName << " collection option is not supported on the "
                              << ns.dbName().toStringForErrorMsg() << " database"};
    }

    return Status::OK();
}

// Validates that the option is not used on admin, local or config db as well as not being used on
// config servers.
Status validateChangeStreamPreAndPostImagesOptionIsPermitted(const NamespaceString& ns) {
    const auto validationStatus =
        validateIsNotInDbs(ns,
                           {DatabaseName::kAdmin, DatabaseName::kLocal, DatabaseName::kConfig},
                           "changeStreamPreAndPostImages");
    if (validationStatus != Status::OK()) {
        return validationStatus;
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
        !gFeatureFlagCatalogShard.isEnabled(serverGlobalParams.featureCompatibility)) {
        return {
            ErrorCodes::InvalidOptions,
            "changeStreamPreAndPostImages collection option is not supported on config servers"};
    }

    return Status::OK();
}

/**
 * Returns true if we are running retryable write or retryable internal multi-document transaction.
 */
bool isRetryableWrite(OperationContext* opCtx) {
    if (!opCtx->writesAreReplicated() || !opCtx->isRetryableWrite()) {
        return false;
    }
    auto txnParticipant = TransactionParticipant::get(opCtx);
    return txnParticipant &&
        (!opCtx->inMultiDocumentTransaction() || txnParticipant.transactionIsOpen());
}

bool indexTypeSupportsPathLevelMultikeyTracking(StringData accessMethod) {
    return accessMethod == IndexNames::BTREE || accessMethod == IndexNames::GEO_2DSPHERE;
}

bool doesMinMaxHaveMixedSchemaData(const BSONObj& min, const BSONObj& max) {
    auto minIt = min.begin();
    auto minEnd = min.end();
    auto maxIt = max.begin();
    auto maxEnd = max.end();

    while (minIt != minEnd && maxIt != maxEnd) {
        bool typeMatch = minIt->canonicalType() == maxIt->canonicalType();
        if (!typeMatch) {
            return true;
        } else if (minIt->type() == Object) {
            // The 'control.min' and 'control.max' fields have the same ordering.
            invariant(minIt->fieldNameStringData() == maxIt->fieldNameStringData());
            if (doesMinMaxHaveMixedSchemaData(minIt->Obj(), maxIt->Obj())) {
                return true;
            }
        } else if (minIt->type() == Array) {
            if (doesMinMaxHaveMixedSchemaData(minIt->Obj(), maxIt->Obj())) {
                return true;
            }
        }

        invariant(typeMatch);
        minIt++;
        maxIt++;
    }

    // The 'control.min' and 'control.max' fields have the same cardinality.
    invariant(minIt == minEnd && maxIt == maxEnd);

    return false;
}

StatusWith<std::shared_ptr<Ident>> findSharedIdentForIndex(OperationContext* opCtx,
                                                           StorageEngine* storageEngine,
                                                           const Collection* collection,
                                                           StringData ident) {
    // First check the index catalog of the existing collection for the index entry.
    auto latestEntry = [&]() -> std::shared_ptr<const IndexCatalogEntry> {
        if (!collection)
            return nullptr;

        auto desc = collection->getIndexCatalog()->findIndexByIdent(opCtx, ident);
        if (!desc)
            return nullptr;
        return collection->getIndexCatalog()->getEntryShared(desc);
    }();

    if (latestEntry) {
        return latestEntry->getSharedIdent();
    }

    // Next check the CollectionCatalog for a compatible drop pending index.
    auto dropPendingEntry = CollectionCatalog::get(opCtx)->findDropPendingIndex(ident);

    // The index entries are incompatible with the read timestamp, but we need to use the same
    // shared ident to prevent the reaper from dropping idents prematurely.
    if (dropPendingEntry) {
        return dropPendingEntry->getSharedIdent();
    }

    // The index ident is expired, but it could still be drop pending. Mark it as in use if
    // possible.
    auto newIdent = storageEngine->markIdentInUse(ident.toString());
    if (newIdent) {
        return newIdent;
    }
    return {ErrorCodes::SnapshotTooOld,
            str::stream() << "Index ident " << ident << " is being dropped or is already dropped."};
}

}  // namespace

std::unique_ptr<CollatorInterface> CollectionImpl::parseCollation(OperationContext* opCtx,
                                                                  const NamespaceString& nss,
                                                                  BSONObj collationSpec) {
    if (collationSpec.isEmpty()) {
        return nullptr;
    }

    auto collator =
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationSpec);

    // If the collection's default collator has a version not currently supported by our ICU
    // integration, shut down the server. Errors other than IncompatibleCollationVersion should not
    // be possible, so these are an invariant rather than fassert.
    if (collator == ErrorCodes::IncompatibleCollationVersion) {
        LOGV2(20288,
              "Collection {namespace} has a default collation which is incompatible with this "
              "version: {collationSpec}"
              "Collection has a default collation incompatible with this version",
              logAttrs(nss),
              "collationSpec"_attr = collationSpec);
        fassertFailedNoTrace(40144);
    }
    invariant(collator.getStatus());

    return std::move(collator.getValue());
}

CollectionImpl::SharedState::SharedState(CollectionImpl* collection,
                                         std::unique_ptr<RecordStore> recordStore,
                                         const CollectionOptions& options)
    : _recordStore(std::move(recordStore)),
      // Capped collections must preserve insertion order, so we serialize writes. One exception are
      // clustered capped collections because they only guarantee insertion order when cluster keys
      // are inserted in monotonically-increasing order.
      _isCapped(options.capped),
      _needCappedLock(_isCapped && collection->ns().isReplicated() && !options.clusteredIndex),
      // The record store will be null when the collection is instantiated as part of the repair
      // path.
      _cappedObserver(_recordStore ? _recordStore->getIdent() : "") {}

CollectionImpl::SharedState::~SharedState() {
    // The record store will be null when the collection is instantiated as part of the repair path.
    // The repair path intentionally doesn't create a record store because it directly accesses the
    // underlying storage engine.
    if (_recordStore && _recordStore->getCappedInsertNotifier()) {
        _recordStore->getCappedInsertNotifier()->kill();
    }
}

CollectionImpl::CollectionImpl(OperationContext* opCtx,
                               const NamespaceString& nss,
                               RecordId catalogId,
                               const CollectionOptions& options,
                               std::unique_ptr<RecordStore> recordStore)
    : _ns(nss),
      _catalogId(std::move(catalogId)),
      _uuid(options.uuid.value()),
      _shared(std::make_shared<SharedState>(this, std::move(recordStore), options)),
      _indexCatalog(std::make_unique<IndexCatalogImpl>()) {}

CollectionImpl::CollectionImpl(OperationContext* opCtx,
                               const NamespaceString& nss,
                               RecordId catalogId,
                               std::shared_ptr<BSONCollectionCatalogEntry::MetaData> metadata,
                               std::unique_ptr<RecordStore> recordStore)
    : CollectionImpl(opCtx, nss, std::move(catalogId), metadata->options, std::move(recordStore)) {
    _metadata = std::move(metadata);
}

CollectionImpl::~CollectionImpl() = default;

void CollectionImpl::onDeregisterFromCatalog(OperationContext* opCtx) {
    if (ns().isOplog()) {
        repl::clearLocalOplogPtr(opCtx->getServiceContext());
    }
}

std::shared_ptr<Collection> CollectionImpl::FactoryImpl::make(
    OperationContext* opCtx,
    const NamespaceString& nss,
    RecordId catalogId,
    const CollectionOptions& options,
    std::unique_ptr<RecordStore> rs) const {
    return std::make_shared<CollectionImpl>(
        opCtx, nss, std::move(catalogId), options, std::move(rs));
}

std::shared_ptr<Collection> CollectionImpl::FactoryImpl::make(
    OperationContext* opCtx,
    const NamespaceString& nss,
    RecordId catalogId,
    std::shared_ptr<BSONCollectionCatalogEntry::MetaData> metadata,
    std::unique_ptr<RecordStore> rs) const {
    return std::make_shared<CollectionImpl>(
        opCtx, nss, std::move(catalogId), std::move(metadata), std::move(rs));
}

std::shared_ptr<Collection> CollectionImpl::clone() const {
    auto cloned = std::make_shared<CollectionImpl>(*this);
    // We are per definition committed if we get cloned
    cloned->_cachedCommitted = true;
    return cloned;
}

SharedCollectionDecorations* CollectionImpl::getSharedDecorations() const {
    return &_shared->_sharedDecorations;
}

void CollectionImpl::init(OperationContext* opCtx) {
    _metadata = DurableCatalog::get(opCtx)->getMetaData(opCtx, getCatalogId());
    const auto& collectionOptions = _metadata->options;

    _initShared(opCtx, collectionOptions);
    _initCommon(opCtx);

    if (collectionOptions.clusteredIndex) {
        if (collectionOptions.expireAfterSeconds) {
            // If this collection has been newly created, we need to register with the TTL cache at
            // commit time, otherwise it is startup and we can register immediately.
            auto svcCtx = opCtx->getClient()->getServiceContext();
            auto uuid = *collectionOptions.uuid;
            if (opCtx->lockState()->inAWriteUnitOfWork()) {
                opCtx->recoveryUnit()->onCommit(
                    [svcCtx, uuid](OperationContext*, boost::optional<Timestamp>) {
                        TTLCollectionCache::get(svcCtx).registerTTLInfo(
                            uuid, TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}});
                    });
            } else {
                TTLCollectionCache::get(svcCtx).registerTTLInfo(
                    uuid, TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}});
            }
        }
    }

    getIndexCatalog()->init(opCtx, this);
    _initialized = true;
}

Status CollectionImpl::initFromExisting(OperationContext* opCtx,
                                        const std::shared_ptr<const Collection>& collection,
                                        const DurableCatalogEntry& catalogEntry,
                                        boost::optional<Timestamp> readTimestamp) {
    // We are per definition committed if we initialize from an existing collection.
    _cachedCommitted = true;

    if (collection) {
        // Use the shared state from the existing collection.
        LOGV2_DEBUG(
            6825402, 1, "Initializing collection using shared state", logAttrs(collection->ns()));
        _shared = static_cast<const CollectionImpl*>(collection.get())->_shared;
    } else {
        _initShared(opCtx, catalogEntry.metadata->options);
    }

    // When initializing a collection from an earlier point-in-time, we don't know when the last DDL
    // operation took place at that point-in-time. We conservatively set the minimum valid snapshot
    // to the read point-in-time.
    _minVisibleSnapshot = readTimestamp;
    _minValidSnapshot = readTimestamp;

    _initCommon(opCtx);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    StringDataMap<std::shared_ptr<Ident>> sharedIdents;

    // Determine which indexes from the existing collection can be shared with this newly
    // initialized collection. The remaining indexes will be initialized by the IndexCatalog.
    auto it = catalogEntry.indexIdents.begin();
    for (size_t offset = 0; offset < _metadata->indexes.size(); ++offset, ++it) {
        invariant(it != catalogEntry.indexIdents.end());

        const auto& index = _metadata->indexes[offset];
        const auto indexName = index.nameStringData();
        if (!isIndexReady(indexName)) {
            continue;
        }

        BSONElement identElem = *it;
        if (indexName != identElem.fieldName()) {
            // If the indexes don't have the same ordering in 'idxIdent' and 'md', we perform a
            // search instead. There's no guarantee these are in order, but they typically are.
            identElem = catalogEntry.indexIdents.getField(indexName);
        }

        auto swIndexIdent = findSharedIdentForIndex(
            opCtx, storageEngine, collection.get(), identElem.checkAndGetStringData());
        if (!swIndexIdent.isOK()) {
            return swIndexIdent.getStatus();
        }
        sharedIdents.emplace(indexName, swIndexIdent.getValue());
    }

    getIndexCatalog()->init(opCtx, this, /*isPointInTimeRead=*/true);

    // Update the idents for the newly initialized indexes. We must reuse the same shared_ptr<Ident>
    // objects from existing indexes to prevent the index idents from being dropped by the drop
    // pending ident reaper while this collection is still using them.
    for (const auto& sharedIdent : sharedIdents) {
        auto desc = getIndexCatalog()->findIndexByName(opCtx, sharedIdent.first);
        invariant(desc);
        auto entry = getIndexCatalog()->getEntryShared(desc);
        entry->setIdent(sharedIdent.second);
    }

    _initialized = true;
    return Status::OK();
}

void CollectionImpl::_initShared(OperationContext* opCtx, const CollectionOptions& options) {
    _shared->_collator = parseCollation(opCtx, _ns, options.collation);
}

void CollectionImpl::_initCommon(OperationContext* opCtx) {
    invariant(!_initialized);

    const auto& collectionOptions = _metadata->options;
    auto validatorDoc = collectionOptions.validator.getOwned();

    // Enforce that the validator can be used on this namespace.
    uassertStatusOK(checkValidatorCanBeUsedOnNs(validatorDoc, _ns, _uuid));

    // Make sure validationAction and validationLevel are allowed on this collection
    uassertStatusOK(checkValidationOptionsCanBeUsed(
        collectionOptions, collectionOptions.validationLevel, collectionOptions.validationAction));

    // Make sure to copy the action and level before parsing MatchExpression, since certain features
    // are not supported with certain combinations of action and level.
    if (collectionOptions.changeStreamPreAndPostImagesOptions.getEnabled()) {
        uassertStatusOK(validateChangeStreamPreAndPostImagesOptionIsPermitted(_ns));
    }

    // Store the result (OK / error) of parsing the validator, but do not enforce that the result is
    // OK. This is intentional, as users may have validators on disk which were considered well
    // formed in older versions but not in newer versions.
    _validator =
        parseValidator(opCtx, validatorDoc, MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!_validator.isOK()) {
        // Log an error and startup warning if the collection validator is malformed.
        LOGV2_WARNING_OPTIONS(20293,
                              {logv2::LogTag::kStartupWarnings},
                              "Collection {namespace} has malformed validator: {validatorStatus}",
                              "Collection has malformed validator",
                              logAttrs(_ns),
                              "validatorStatus"_attr = _validator.getStatus());
    }
}

bool CollectionImpl::isInitialized() const {
    return _initialized;
}

bool CollectionImpl::isCommitted() const {
    return _cachedCommitted || _shared->_committed.load();
}

void CollectionImpl::setCommitted(bool val) {
    bool previous = isCommitted();
    invariant((!previous && val) || (previous && !val));
    _shared->_committed.store(val);

    // Going from false->true need to be synchronized by an atomic. Leave this as false and read
    // from the atomic in the shared state that will be flipped to true at first clone.
    if (!val) {
        _cachedCommitted = val;
    }
}

bool CollectionImpl::requiresIdIndex() const {
    if (_ns.isOplog()) {
        // No indexes on the oplog.
        return false;
    }

    if (isClustered()) {
        // Collections clustered by _id do not have a separate _id index.
        return false;
    }

    if (_ns.isSystem()) {
        StringData shortName = _ns.coll().substr(_ns.coll().find('.') + 1);
        if (shortName == "indexes" || shortName == "namespaces" || shortName == "profile") {
            return false;
        }
    }

    return true;
}

std::unique_ptr<SeekableRecordCursor> CollectionImpl::getCursor(OperationContext* opCtx,
                                                                bool forward) const {
    if (usesCappedSnapshots() && forward) {
        if (opCtx->recoveryUnit()->isActive()) {
            auto snapshot =
                CappedSnapshots::get(opCtx).getSnapshot(_shared->_recordStore->getIdent());
            invariant(
                CollectionCatalog::hasExclusiveAccessToCollection(opCtx, ns()) || snapshot,
                fmt::format("Capped visibility snapshot was not initialized before reading from "
                            "collection non-exclusively: {}",
                            _ns.toStringForErrorMsg()));
        } else {
            // We can lazily initialize the capped snapshot because no storage snapshot has been
            // opened yet.
            CappedSnapshots::get(opCtx).establish(opCtx, this);
        }
    }
    return _shared->_recordStore->getCursor(opCtx, forward);
}


bool CollectionImpl::findDoc(OperationContext* opCtx,
                             const RecordId& loc,
                             Snapshotted<BSONObj>* out) const {
    RecordData rd;
    if (!_shared->_recordStore->findRecord(opCtx, loc, &rd))
        return false;
    *out = Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(), rd.releaseToBson());
    return true;
}

Status CollectionImpl::checkValidatorAPIVersionCompatability(OperationContext* opCtx) const {
    if (!_validator.expCtxForFilter) {
        return Status::OK();
    }
    const auto& apiParams = APIParameters::get(opCtx);
    const auto apiVersion = apiParams.getAPIVersion().value_or("");
    if (apiParams.getAPIStrict().value_or(false) && apiVersion == "1" &&
        _validator.expCtxForFilter->exprUnstableForApiV1) {
        return {ErrorCodes::APIStrictError,
                "The validator uses unstable expression(s) for API Version 1."};
    }
    if (apiParams.getAPIDeprecationErrors().value_or(false) && apiVersion == "1" &&
        _validator.expCtxForFilter->exprDeprectedForApiV1) {
        return {ErrorCodes::APIDeprecationError,
                "The validator uses deprecated expression(s) for API Version 1."};
    }
    return Status::OK();
}

std::pair<Collection::SchemaValidationResult, Status> CollectionImpl::checkValidation(
    OperationContext* opCtx, const BSONObj& document) const {
    if (!_validator.isOK()) {
        return {SchemaValidationResult::kError, _validator.getStatus()};
    }

    const auto* const validatorMatchExpr = _validator.filter.getValue().get();
    if (!validatorMatchExpr)
        return {SchemaValidationResult::kPass, Status::OK()};

    if (validationLevelOrDefault(_metadata->options.validationLevel) == ValidationLevelEnum::off)
        return {SchemaValidationResult::kPass, Status::OK()};

    if (DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled())
        return {SchemaValidationResult::kPass, Status::OK()};

    if (ns().isTemporaryReshardingCollection()) {
        // In resharding, the donor shard primary is responsible for performing document validation
        // and the recipient should not perform validation on documents inserted into the temporary
        // resharding collection.
        return {SchemaValidationResult::kPass, Status::OK()};
    }

    auto status = checkValidatorAPIVersionCompatability(opCtx);
    if (!status.isOK()) {
        return {SchemaValidationResult::kError, status};
    }

    try {
        if (validatorMatchExpr->matchesBSON(document))
            return {SchemaValidationResult::kPass, Status::OK()};
    } catch (DBException&) {
    };

    BSONObj generatedError = doc_validation_error::generateError(*validatorMatchExpr, document);

    static constexpr auto kValidationFailureErrorStr = "Document failed validation"_sd;
    status = Status(doc_validation_error::DocumentValidationFailureInfo(generatedError),
                    kValidationFailureErrorStr);

    if (validationActionOrDefault(_metadata->options.validationAction) ==
        ValidationActionEnum::warn) {
        return {SchemaValidationResult::kWarn, status};
    }

    return {SchemaValidationResult::kError, status};
}

Status CollectionImpl::checkValidationAndParseResult(OperationContext* opCtx,
                                                     const BSONObj& document) const {
    std::pair<SchemaValidationResult, Status> result = checkValidation(opCtx, document);

    if (result.first == SchemaValidationResult::kPass) {
        return Status::OK();
    }

    if (result.first == SchemaValidationResult::kWarn) {
        LOGV2_WARNING(
            20294,
            "Document would fail validation",
            logAttrs(ns()),
            "document"_attr = redact(document),
            "errInfo"_attr =
                result.second.extraInfo<doc_validation_error::DocumentValidationFailureInfo>()
                    ->getDetails());
        return Status::OK();
    }

    return result.second;
}

Collection::Validator CollectionImpl::parseValidator(
    OperationContext* opCtx,
    const BSONObj& validator,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    boost::optional<multiversion::FeatureCompatibilityVersion> maxFeatureCompatibilityVersion)
    const {
    if (MONGO_unlikely(allowSettingMalformedCollectionValidators.shouldFail())) {
        return {validator, nullptr, nullptr};
    }

    bool doImplicitValidation = (_metadata->options.encryptedFieldConfig != boost::none) &&
        !_metadata->options.encryptedFieldConfig->getFields().empty();

    if (validator.isEmpty() && !doImplicitValidation) {
        return {validator, nullptr, nullptr};
    }

    Status canUseValidatorInThisContext = checkValidatorCanBeUsedOnNs(validator, ns(), _uuid);
    if (!canUseValidatorInThisContext.isOK()) {
        return {validator, nullptr, canUseValidatorInThisContext};
    }

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, CollatorInterface::cloneCollator(_shared->_collator.get()), ns());

    expCtx->variables.setDefaultRuntimeConstants(opCtx);

    // The MatchExpression and contained ExpressionContext created as part of the validator are
    // owned by the Collection and will outlive the OperationContext they were created under.
    expCtx->opCtx = nullptr;

    // Enforce a maximum feature version if requested.
    expCtx->maxFeatureCompatibilityVersion = maxFeatureCompatibilityVersion;

    // The match expression parser needs to know that we're parsing an expression for a
    // validator to apply some additional checks.
    expCtx->isParsingCollectionValidator = true;

    // If the validation action is "warn" or the level is "moderate", then disallow any encryption
    // keywords. This is to prevent any plaintext data from showing up in the logs.
    // Also disallow if the collection has FLE2 encrypted fields.
    if (validationActionOrDefault(_metadata->options.validationAction) ==
            ValidationActionEnum::warn ||
        validationLevelOrDefault(_metadata->options.validationLevel) ==
            ValidationLevelEnum::moderate ||
        doImplicitValidation)
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

    std::unique_ptr<MatchExpression> implicitMatchExpr;
    std::unique_ptr<MatchExpression> explicitMatchExpr;
    std::unique_ptr<MatchExpression> combinedMatchExpr;

    if (doImplicitValidation) {
        auto statusWithMatcher = generateMatchExpressionFromEncryptedFields(
            expCtx, _metadata->options.encryptedFieldConfig->getFields());
        if (!statusWithMatcher.isOK()) {
            return {validator,
                    nullptr,
                    statusWithMatcher.getStatus().withContext(
                        "Failed to generate implicit validator for encrypted fields")};
        }
        implicitMatchExpr = std::move(statusWithMatcher.getValue());
    }

    if (!validator.isEmpty()) {
        expCtx->startExpressionCounters();
        auto statusWithMatcher = MatchExpressionParser::parse(
            validator, expCtx, ExtensionsCallbackNoop(), allowedFeatures);
        expCtx->stopExpressionCounters();

        if (!statusWithMatcher.isOK()) {
            return {validator,
                    boost::intrusive_ptr<ExpressionContext>(nullptr),
                    statusWithMatcher.getStatus().withContext(
                        "Parsing of collection validator failed")};
        }
        explicitMatchExpr = std::move(statusWithMatcher.getValue());
    }

    if (implicitMatchExpr && explicitMatchExpr) {
        combinedMatchExpr = std::make_unique<AndMatchExpression>(
            makeVector(std::move(explicitMatchExpr), std::move(implicitMatchExpr)),
            doc_validation_error::createAnnotation(expCtx, "$and", BSONObj()));
    } else if (implicitMatchExpr) {
        combinedMatchExpr = std::move(implicitMatchExpr);
    } else {
        combinedMatchExpr = std::move(explicitMatchExpr);
    }

    LOGV2_DEBUG(6364301,
                5,
                "Combined match expression",
                "expression"_attr = combinedMatchExpr->serialize());

    return Collection::Validator{validator, std::move(expCtx), std::move(combinedMatchExpr)};
}

bool CollectionImpl::needsCappedLock() const {
    return _shared->_needCappedLock;
}

bool CollectionImpl::isCappedAndNeedsDelete(OperationContext* opCtx) const {
    if (MONGO_unlikely(skipCappedDeletes.shouldFail())) {
        return false;
    }

    if (!isCapped()) {
        return false;
    }

    if (getClusteredInfo()) {
        // Capped clustered collections use TTL-based deletion.
        return false;
    }

    if (ns().isOplog() && getRecordStore()->selfManagedOplogTruncation()) {
        // Storage engines can choose to manage oplog truncation internally.
        return false;
    }

    if (dataSize(opCtx) > getCollectionOptions().cappedSize) {
        return true;
    }

    const auto cappedMaxDocs = getCollectionOptions().cappedMaxDocs;
    if ((cappedMaxDocs != 0) && (numRecords(opCtx) > cappedMaxDocs)) {
        return true;
    }

    return false;
}

void CollectionImpl::setMinimumVisibleSnapshot(Timestamp newMinimumVisibleSnapshot) {
    if (!_minVisibleSnapshot || (newMinimumVisibleSnapshot > _minVisibleSnapshot.value())) {
        _minVisibleSnapshot = newMinimumVisibleSnapshot;
    }
}

void CollectionImpl::setMinimumValidSnapshot(Timestamp newMinimumValidSnapshot) {
    if (!_minValidSnapshot || (newMinimumValidSnapshot > _minValidSnapshot.value())) {
        _minValidSnapshot = newMinimumValidSnapshot;
    }
}

bool CollectionImpl::updateWithDamagesSupported() const {
    if (!_validator.isOK() || _validator.filter.getValue() != nullptr)
        return false;

    return _shared->_recordStore->updateWithDamagesSupported();
}

bool CollectionImpl::isTemporary() const {
    return _metadata->options.temp;
}

boost::optional<bool> CollectionImpl::getTimeseriesBucketsMayHaveMixedSchemaData() const {
    return _metadata->timeseriesBucketsMayHaveMixedSchemaData;
}

void CollectionImpl::setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext* opCtx,
                                                                boost::optional<bool> setting) {
    uassert(6057500, "This is not a time-series collection", _metadata->options.timeseries);

    LOGV2_DEBUG(6057601,
                1,
                "Setting 'timeseriesBucketsMayHaveMixedSchemaData' catalog entry flag",
                logAttrs(ns()),
                logAttrs(uuid()),
                "setting"_attr = setting);

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.timeseriesBucketsMayHaveMixedSchemaData = setting;
    });
}

bool CollectionImpl::doesTimeseriesBucketsDocContainMixedSchemaData(
    const BSONObj& bucketsDoc) const {
    if (!getTimeseriesOptions()) {
        return false;
    }

    const BSONObj controlObj = bucketsDoc.getObjectField(timeseries::kBucketControlFieldName);
    const BSONObj minObj = controlObj.getObjectField(timeseries::kBucketControlMinFieldName);
    const BSONObj maxObj = controlObj.getObjectField(timeseries::kBucketControlMaxFieldName);

    return doesMinMaxHaveMixedSchemaData(minObj, maxObj);
}

bool CollectionImpl::getRequiresTimeseriesExtendedRangeSupport() const {
    return _shared->_requiresTimeseriesExtendedRangeSupport.load();
}

void CollectionImpl::setRequiresTimeseriesExtendedRangeSupport(OperationContext* opCtx) const {
    uassert(6679401, "This is not a time-series collection", _metadata->options.timeseries);

    bool expected = false;
    bool set = _shared->_requiresTimeseriesExtendedRangeSupport.compareAndSwap(&expected, true);
    if (set) {
        catalog_stats::requiresTimeseriesExtendedRangeSupport.fetchAndAdd(1);
        if (!timeseries::collectionHasTimeIndex(opCtx, *this)) {
            LOGV2_WARNING(
                6679402,
                "Time-series collection contains dates outside the standard range. Some query "
                "optimizations may be disabled. Please consider building an index on timeField to "
                "re-enable them.",
                "nss"_attr = ns().getTimeseriesViewNamespace(),
                "timeField"_attr = _metadata->options.timeseries->getTimeField());
        }
    }
}

bool CollectionImpl::isClustered() const {
    return getClusteredInfo().has_value();
}

boost::optional<ClusteredCollectionInfo> CollectionImpl::getClusteredInfo() const {
    return getCollectionOptions().clusteredIndex;
}

void CollectionImpl::updateClusteredIndexTTLSetting(OperationContext* opCtx,
                                                    boost::optional<int64_t> expireAfterSeconds) {
    uassert(5401000,
            "The collection doesn't have a clustered index",
            _metadata->options.clusteredIndex);

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.options.expireAfterSeconds = expireAfterSeconds;
    });
}

Status CollectionImpl::updateCappedSize(OperationContext* opCtx,
                                        boost::optional<long long> newCappedSize,
                                        boost::optional<long long> newCappedMax) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    if (!_shared->_isCapped) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Cannot update size on a non-capped collection "
                                    << ns().toStringForErrorMsg());
    }

    if (ns().isOplog() && newCappedSize) {
        Status status = _shared->_recordStore->updateOplogSize(opCtx, *newCappedSize);
        if (!status.isOK()) {
            return status;
        }
    }

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        if (newCappedSize) {
            md.options.cappedSize = *newCappedSize;
        }
        if (newCappedMax) {
            md.options.cappedMaxDocs = *newCappedMax;
        }
    });
    return Status::OK();
}

bool CollectionImpl::isChangeStreamPreAndPostImagesEnabled() const {
    return _metadata->options.changeStreamPreAndPostImagesOptions.getEnabled();
}

void CollectionImpl::setChangeStreamPreAndPostImages(OperationContext* opCtx,
                                                     ChangeStreamPreAndPostImagesOptions val) {
    if (val.getEnabled()) {
        uassertStatusOK(validateChangeStreamPreAndPostImagesOptionIsPermitted(_ns));
    }

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.options.changeStreamPreAndPostImagesOptions = val;
    });
}

bool CollectionImpl::isCapped() const {
    return _shared->_isCapped;
}

long long CollectionImpl::getCappedMaxDocs() const {
    return _metadata->options.cappedMaxDocs;
}

long long CollectionImpl::getCappedMaxSize() const {
    return _metadata->options.cappedSize;
}

bool CollectionImpl::usesCappedSnapshots() const {
    // Only use the behavior for non-replicated capped collections (which can accept concurrent
    // writes). This behavior relies on RecordIds being allocated in increasing order. For clustered
    // collections, users define their RecordIds and are not constrained to creating them in
    // increasing order.
    // The oplog tracks its visibility through support from the storage engine.
    return isCapped() && !ns().isReplicated() && !ns().isOplog() && !isClustered();
}

CappedVisibilityObserver* CollectionImpl::getCappedVisibilityObserver() const {
    invariant(usesCappedSnapshots());
    return &_shared->_cappedObserver;
}

std::vector<RecordId> CollectionImpl::reserveCappedRecordIds(OperationContext* opCtx,
                                                             size_t count) const {
    invariant(usesCappedSnapshots());

    // By registering ourselves as a writer, we inform the capped visibility system that we may be
    // in the process of committing uncommitted records.
    auto cappedObserver = getCappedVisibilityObserver();
    cappedObserver->registerWriter(
        opCtx->recoveryUnit(), [this]() { _shared->_recordStore->notifyCappedWaitersIfNeeded(); });

    std::vector<RecordId> ids;
    ids.reserve(count);
    {
        // We must atomically allocate and register any RecordIds so that we can correctly keep
        // track of visibility. This ensures capped readers do not skip past any in-progress writes.
        stdx::lock_guard<Latch> lk(_shared->_registerCappedIdsMutex);
        _shared->_recordStore->reserveRecordIds(opCtx, &ids, count);

        // We are guaranteed to have a contiguous range so we only register the min and max.
        registerCappedInserts(opCtx, ids.front(), ids.back());
    }

    return ids;
}

void CollectionImpl::registerCappedInserts(OperationContext* opCtx,
                                           const RecordId& minRecord,
                                           const RecordId& maxRecord) const {
    invariant(usesCappedSnapshots());
    // Callers should be updating visibility as part of a write operation. We want to ensure that
    // we never get here while holding an uninterruptible, read-ticketed lock. That would indicate
    // that we are operating with the wrong global lock semantics, and either hold too weak a lock
    // (e.g. IS) or that we upgraded in a way we shouldn't (e.g. IS -> IX).
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->hasReadTicket() ||
              !opCtx->lockState()->uninterruptibleLocksRequested());

    auto* uncommitted =
        CappedWriter::get(opCtx).getUncommitedRecordsFor(_shared->_recordStore->getIdent());
    uncommitted->registerRecordIds(minRecord, maxRecord);
    return;
}

CappedVisibilitySnapshot CollectionImpl::takeCappedVisibilitySnapshot() const {
    invariant(usesCappedSnapshots());
    return _shared->_cappedObserver.makeSnapshot();
}

long long CollectionImpl::numRecords(OperationContext* opCtx) const {
    return _shared->_recordStore->numRecords(opCtx);
}

long long CollectionImpl::dataSize(OperationContext* opCtx) const {
    return _shared->_recordStore->dataSize(opCtx);
}

bool CollectionImpl::isEmpty(OperationContext* opCtx) const {
    auto cursor = getCursor(opCtx, true /* forward */);

    auto cursorEmptyCollRes = (!cursor->next()) ? true : false;
    auto fastCount = numRecords(opCtx);
    auto fastCountEmptyCollRes = (fastCount == 0) ? true : false;

    if (cursorEmptyCollRes != fastCountEmptyCollRes) {
        BSONObjBuilder bob;
        bob.appendNumber("fastCount", static_cast<long long>(fastCount));
        bob.append("cursor", str::stream() << (cursorEmptyCollRes ? "0" : ">=1"));

        LOGV2_DEBUG(20292,
                    2,
                    "Detected erroneous fast count for collection {namespace}({uuid}) "
                    "[{getRecordStore_getIdent}]. Record count reported by: {bob_obj}",
                    logAttrs(ns()),
                    "uuid"_attr = uuid(),
                    "getRecordStore_getIdent"_attr = getRecordStore()->getIdent(),
                    "bob_obj"_attr = bob.obj());
    }

    return cursorEmptyCollRes;
}

uint64_t CollectionImpl::getIndexSize(OperationContext* opCtx,
                                      BSONObjBuilder* details,
                                      int scale) const {
    const IndexCatalog* idxCatalog = getIndexCatalog();

    auto ii = idxCatalog->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);

    uint64_t totalSize = 0;

    while (ii->more()) {
        const IndexCatalogEntry* entry = ii->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        long long ds = iam->getSpaceUsedBytes(opCtx);

        totalSize += ds;
        if (details) {
            details->appendNumber(descriptor->indexName(), ds / scale);
        }
    }

    return totalSize;
}

uint64_t CollectionImpl::getIndexFreeStorageBytes(OperationContext* const opCtx) const {
    const auto idxCatalog = getIndexCatalog();
    auto indexIt = idxCatalog->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);

    uint64_t totalSize = 0;
    while (indexIt->more()) {
        auto entry = indexIt->next();
        totalSize += entry->accessMethod()->getFreeStorageBytes(opCtx);
    }
    return totalSize;
}

/**
 * order will be:
 * 1) store index specs
 * 2) drop indexes
 * 3) truncate record store
 * 4) re-write indexes
 */
Status CollectionImpl::truncate(OperationContext* opCtx) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));
    invariant(_indexCatalog->numIndexesInProgress() == 0);

    // 1) store index specs
    std::vector<BSONObj> indexSpecs;
    {
        auto ii = _indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
        while (ii->more()) {
            const IndexDescriptor* idx = ii->next()->descriptor();
            indexSpecs.push_back(idx->infoObj().getOwned());
        }
    }

    // 2) drop indexes
    _indexCatalog->dropAllIndexes(opCtx, this, true, {});

    // 3) truncate record store
    auto status = _shared->_recordStore->truncate(opCtx);
    if (!status.isOK())
        return status;

    // 4) re-create indexes
    for (size_t i = 0; i < indexSpecs.size(); i++) {
        status =
            _indexCatalog->createIndexOnEmptyCollection(opCtx, this, indexSpecs[i]).getStatus();
        if (!status.isOK())
            return status;
    }

    return Status::OK();
}

void CollectionImpl::setValidator(OperationContext* opCtx, Validator validator) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    auto validatorDoc = validator.validatorDoc.getOwned();
    auto validationLevel = validationLevelOrDefault(_metadata->options.validationLevel);
    auto validationAction = validationActionOrDefault(_metadata->options.validationAction);

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.options.validator = validatorDoc;
        md.options.validationLevel = validationLevel;
        md.options.validationAction = validationAction;
    });

    _validator = std::move(validator);
}

boost::optional<ValidationLevelEnum> CollectionImpl::getValidationLevel() const {
    return _metadata->options.validationLevel;
}

boost::optional<ValidationActionEnum> CollectionImpl::getValidationAction() const {
    return _metadata->options.validationAction;
}

Status CollectionImpl::setValidationLevel(OperationContext* opCtx, ValidationLevelEnum newLevel) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    auto status = checkValidationOptionsCanBeUsed(_metadata->options, newLevel, boost::none);
    if (!status.isOK()) {
        return status;
    }

    auto storedValidationLevel = validationLevelOrDefault(newLevel);

    // Reparse the validator as there are some features which are only supported with certain
    // validation levels.
    auto allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures;
    if (storedValidationLevel == ValidationLevelEnum::moderate)
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

    _validator = parseValidator(opCtx, _validator.validatorDoc, allowedFeatures);
    if (!_validator.isOK()) {
        return _validator.getStatus();
    }

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.options.validator = _validator.validatorDoc;
        md.options.validationLevel = storedValidationLevel;
        md.options.validationAction = validationActionOrDefault(md.options.validationAction);
    });

    return Status::OK();
}

Status CollectionImpl::setValidationAction(OperationContext* opCtx,
                                           ValidationActionEnum newAction) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    auto status = checkValidationOptionsCanBeUsed(_metadata->options, boost::none, newAction);
    if (!status.isOK()) {
        return status;
    }

    auto storedValidationAction = validationActionOrDefault(newAction);

    // Reparse the validator as there are some features which are only supported with certain
    // validation actions.
    auto allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures;
    if (storedValidationAction == ValidationActionEnum::warn)
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

    _validator = parseValidator(opCtx, _validator.validatorDoc, allowedFeatures);
    if (!_validator.isOK()) {
        return _validator.getStatus();
    }

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.options.validator = _validator.validatorDoc;
        md.options.validationLevel = validationLevelOrDefault(md.options.validationLevel);
        md.options.validationAction = storedValidationAction;
    });

    return Status::OK();
}

Status CollectionImpl::updateValidator(OperationContext* opCtx,
                                       BSONObj newValidator,
                                       boost::optional<ValidationLevelEnum> newLevel,
                                       boost::optional<ValidationActionEnum> newAction) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    auto status = checkValidationOptionsCanBeUsed(_metadata->options, newLevel, newAction);
    if (!status.isOK()) {
        return status;
    }

    auto validator =
        parseValidator(opCtx, newValidator, MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!validator.isOK()) {
        return validator.getStatus();
    }

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.options.validator = newValidator;
        md.options.validationLevel = newLevel;
        md.options.validationAction = newAction;
    });

    _validator = std::move(validator);
    return Status::OK();
}

boost::optional<TimeseriesOptions> CollectionImpl::getTimeseriesOptions() const {
    return _metadata->options.timeseries;
}

void CollectionImpl::setTimeseriesOptions(OperationContext* opCtx,
                                          const TimeseriesOptions& tsOptions) {
    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.options.timeseries = tsOptions;
    });
}

const CollatorInterface* CollectionImpl::getDefaultCollator() const {
    return _shared->_collator.get();
}

const CollectionOptions& CollectionImpl::getCollectionOptions() const {
    return _metadata->options;
}

StatusWith<std::vector<BSONObj>> CollectionImpl::addCollationDefaultsToIndexSpecsForCreate(
    OperationContext* opCtx, const std::vector<BSONObj>& originalIndexSpecs) const {
    std::vector<BSONObj> newIndexSpecs;

    auto collator = getDefaultCollator();  // could be null.
    auto collatorFactory = CollatorFactoryInterface::get(opCtx->getServiceContext());

    for (const auto& originalIndexSpec : originalIndexSpecs) {
        auto validateResult =
            index_key_validate::validateIndexSpecCollation(opCtx, originalIndexSpec, collator);
        if (!validateResult.isOK()) {
            return validateResult.getStatus().withContext(
                str::stream()
                << "failed to add collation information to index spec for index creation: "
                << originalIndexSpec);
        }
        BSONObj newIndexSpec = validateResult.getValue();

        auto keyPattern = newIndexSpec[IndexDescriptor::kKeyPatternFieldName].Obj();
        if (IndexDescriptor::isIdIndexPattern(keyPattern)) {
            std::unique_ptr<CollatorInterface> indexCollator;
            if (auto collationElem = newIndexSpec[IndexDescriptor::kCollationFieldName]) {
                auto indexCollatorResult = collatorFactory->makeFromBSON(collationElem.Obj());
                // validateIndexSpecCollation() should have checked that the index collation spec is
                // valid.
                invariant(indexCollatorResult.getStatus(),
                          str::stream() << "invalid collation in index spec: " << newIndexSpec);
                indexCollator = std::move(indexCollatorResult.getValue());
            }
            if (!CollatorInterface::collatorsMatch(collator, indexCollator.get())) {
                return {ErrorCodes::BadValue,
                        str::stream() << "The _id index must have the same collation as the "
                                         "collection. Index collation: "
                                      << (indexCollator.get() ? indexCollator->getSpec().toBSON()
                                                              : CollationSpec::kSimpleSpec)
                                      << ", collection collation: "
                                      << (collator ? collator->getSpec().toBSON()
                                                   : CollationSpec::kSimpleSpec)};
            }
        }

        if (originalIndexSpec.hasField(IndexDescriptor::kOriginalSpecFieldName)) {
            // Validation was already performed above.
            BSONObj newOriginalIndexSpec = invariant(index_key_validate::validateIndexSpecCollation(
                opCtx,
                originalIndexSpec.getObjectField(IndexDescriptor::kOriginalSpecFieldName),
                collator));

            BSONObj specToAdd =
                BSON(IndexDescriptor::kOriginalSpecFieldName << newOriginalIndexSpec);
            newIndexSpec = newIndexSpec.addField(specToAdd.firstElement());
        }

        newIndexSpecs.push_back(newIndexSpec);
    }

    return newIndexSpecs;
}

Status CollectionImpl::rename(OperationContext* opCtx, const NamespaceString& nss, bool stayTemp) {
    auto metadata = std::make_shared<BSONCollectionCatalogEntry::MetaData>(*_metadata);
    metadata->nss = nss;
    if (!stayTemp)
        metadata->options.temp = false;
    Status status =
        DurableCatalog::get(opCtx)->renameCollection(opCtx, getCatalogId(), nss, *metadata);
    if (!status.isOK()) {
        return status;
    }

    _metadata = std::move(metadata);
    _ns = std::move(nss);
    return status;
}

void CollectionImpl::indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) {
    const auto& indexName = index->descriptor()->indexName();
    int offset = _metadata->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot mark index " << indexName << " as ready @ " << getCatalogId()
                            << " : " << _metadata->toBSON());

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.indexes[offset].ready = true;
        md.indexes[offset].buildUUID = boost::none;
    });

    _indexCatalog->indexBuildSuccess(opCtx, this, index);
}

StatusWith<int> CollectionImpl::checkMetaDataForIndex(const std::string& indexName,
                                                      const BSONObj& spec) const {
    int offset = _metadata->findIndexOffset(indexName);
    if (offset < 0) {
        return {ErrorCodes::IndexNotFound,
                str::stream() << "Index [" << indexName
                              << "] not found in metadata for recordId: " << getCatalogId()};
    }

    if (spec.woCompare(_metadata->indexes[offset].spec)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Spec for index [" << indexName
                              << "] does not match spec in the metadata for recordId: "
                              << getCatalogId() << ". Spec: " << spec
                              << " metadata's spec: " << _metadata->indexes[offset].spec};
    }

    return offset;
}

void CollectionImpl::updateTTLSetting(OperationContext* opCtx,
                                      StringData idxName,
                                      long long newExpireSeconds) {
    int offset = _metadata->findIndexOffset(idxName);
    invariant(offset >= 0,
              str::stream() << "cannot update TTL setting for index " << idxName << " @ "
                            << getCatalogId() << " : " << _metadata->toBSON());
    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.indexes[offset].updateTTLSetting(newExpireSeconds);
    });
}

void CollectionImpl::updateHiddenSetting(OperationContext* opCtx, StringData idxName, bool hidden) {
    int offset = _metadata->findIndexOffset(idxName);
    invariant(offset >= 0);

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.indexes[offset].updateHiddenSetting(hidden);
    });
}

void CollectionImpl::updateUniqueSetting(OperationContext* opCtx, StringData idxName, bool unique) {
    int offset = _metadata->findIndexOffset(idxName);
    invariant(offset >= 0);

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.indexes[offset].updateUniqueSetting(unique);
    });
}

void CollectionImpl::updatePrepareUniqueSetting(OperationContext* opCtx,
                                                StringData idxName,
                                                bool prepareUnique) {
    int offset = _metadata->findIndexOffset(idxName);
    invariant(offset >= 0);

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        md.indexes[offset].updatePrepareUniqueSetting(prepareUnique);
    });
}

std::vector<std::string> CollectionImpl::repairInvalidIndexOptions(OperationContext* opCtx) {
    std::vector<std::string> indexesWithInvalidOptions;

    _writeMetadata(opCtx, [&](BSONCollectionCatalogEntry::MetaData& md) {
        for (auto& index : md.indexes) {
            if (index.isPresent()) {
                BSONObj oldSpec = index.spec;

                Status status = index_key_validate::validateIndexSpec(opCtx, oldSpec).getStatus();
                if (status.isOK()) {
                    continue;
                }

                indexesWithInvalidOptions.push_back(std::string(index.nameStringData()));
                index.spec = index_key_validate::repairIndexSpec(md.nss, oldSpec);
            }
        }
    });

    return indexesWithInvalidOptions;
}

void CollectionImpl::setIsTemp(OperationContext* opCtx, bool isTemp) {
    _writeMetadata(opCtx,
                   [&](BSONCollectionCatalogEntry::MetaData& md) { md.options.temp = isTemp; });
}

void CollectionImpl::removeIndex(OperationContext* opCtx, StringData indexName) {
    if (_metadata->findIndexOffset(indexName) < 0)
        return;  // never had the index so nothing to do.

    _writeMetadata(opCtx,
                   [&](BSONCollectionCatalogEntry::MetaData& md) { md.eraseIndex(indexName); });
}

Status CollectionImpl::prepareForIndexBuild(OperationContext* opCtx,
                                            const IndexDescriptor* spec,
                                            boost::optional<UUID> buildUUID,
                                            bool isBackgroundSecondaryBuild) {

    auto durableCatalog = DurableCatalog::get(opCtx);
    BSONCollectionCatalogEntry::IndexMetaData imd;
    imd.spec = spec->infoObj();
    imd.ready = false;
    imd.multikey = false;
    imd.isBackgroundSecondaryBuild = isBackgroundSecondaryBuild;
    imd.buildUUID = buildUUID;

    if (indexTypeSupportsPathLevelMultikeyTracking(spec->getAccessMethodName())) {
        imd.multikeyPaths = MultikeyPaths{static_cast<size_t>(spec->keyPattern().nFields())};
    }

    // Confirm that our index is not already in the current metadata.
    invariant(-1 == _metadata->findIndexOffset(imd.nameStringData()),
              str::stream() << "index " << imd.nameStringData()
                            << " is already in current metadata: " << _metadata->toBSON());

    if (getTimeseriesOptions() &&
        feature_flags::gTimeseriesMetricIndexes.isEnabled(
            serverGlobalParams.featureCompatibility) &&
        timeseries::doesBucketsIndexIncludeMeasurement(
            opCtx, ns(), *getTimeseriesOptions(), spec->infoObj())) {
        invariant(_metadata->timeseriesBucketsMayHaveMixedSchemaData);
        if (*_metadata->timeseriesBucketsMayHaveMixedSchemaData) {
            LOGV2(6057502,
                  "Detected that this time-series collection may have mixed-schema data. "
                  "Attempting to build the index.",
                  logAttrs(ns()),
                  logAttrs(uuid()),
                  "spec"_attr = spec->infoObj());
        }
    }

    _writeMetadata(opCtx,
                   [indexMetaData = std::move(imd)](BSONCollectionCatalogEntry::MetaData& md) {
                       md.insertIndex(std::move(indexMetaData));
                   });

    return durableCatalog->createIndex(opCtx, getCatalogId(), ns(), getCollectionOptions(), spec);
}

boost::optional<UUID> CollectionImpl::getIndexBuildUUID(StringData indexName) const {
    int offset = _metadata->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot get build UUID for index " << indexName << " @ "
                            << getCatalogId() << " : " << _metadata->toBSON());
    return _metadata->indexes[offset].buildUUID;
}

bool CollectionImpl::isIndexMultikey(OperationContext* opCtx,
                                     StringData indexName,
                                     MultikeyPaths* multikeyPaths,
                                     int indexOffset) const {
    int offset = indexOffset;
    if (offset < 0) {
        offset = _metadata->findIndexOffset(indexName);
        invariant(offset >= 0,
                  str::stream() << "cannot get multikey for index " << indexName << " @ "
                                << getCatalogId() << " : " << _metadata->toBSON());
    } else {
        invariant(offset < int(_metadata->indexes.size()),
                  str::stream() << "out of bounds index offset for multikey info " << indexName
                                << " @ " << getCatalogId() << " : " << _metadata->toBSON()
                                << "; offset : " << offset
                                << " ; actual : " << _metadata->findIndexOffset(indexName));
        invariant(indexName == _metadata->indexes[offset].nameStringData(),
                  str::stream() << "invalid index offset for multikey info " << indexName << " @ "
                                << getCatalogId() << " : " << _metadata->toBSON()
                                << "; offset : " << offset
                                << " ; actual : " << _metadata->findIndexOffset(indexName));
    }

    // If we have uncommitted multikey writes we need to check here to read our own writes
    const auto& uncommittedMultikeys = UncommittedMultikey::get(opCtx).resources();
    if (uncommittedMultikeys) {
        if (auto it = uncommittedMultikeys->find(this); it != uncommittedMultikeys->end()) {
            const auto& index = it->second.indexes[offset];
            if (multikeyPaths && !index.multikeyPaths.empty()) {
                *multikeyPaths = index.multikeyPaths;
            }
            return index.multikey;
        }
    }

    // Otherwise read from the metadata cache if there are no concurrent multikey writers
    {
        const auto& index = _metadata->indexes[offset];
        // Check for concurrent writers, this can race with writers where it can be set immediately
        // after checking. This is fine we know that the reader in that case opened its snapshot
        // before the writer and we do not need to observe its result.
        if (index.concurrentWriters.load() == 0) {
            stdx::lock_guard lock(index.multikeyMutex);
            if (multikeyPaths && !index.multikeyPaths.empty()) {
                *multikeyPaths = index.multikeyPaths;
            }
            return index.multikey;
        }
    }

    // We need to read from the durable catalog if there are concurrent multikey writers to avoid
    // reading between the multikey write committing in the storage engine but before its onCommit
    // handler made the write visible for readers.
    auto snapshotMetadata = DurableCatalog::get(opCtx)->getMetaData(opCtx, getCatalogId());
    int snapshotOffset = snapshotMetadata->findIndexOffset(indexName);
    invariant(snapshotOffset >= 0,
              str::stream() << "cannot get multikey for index " << indexName << " @ "
                            << getCatalogId() << " : " << _metadata->toBSON());
    const auto& index = snapshotMetadata->indexes[snapshotOffset];
    if (multikeyPaths && !index.multikeyPaths.empty()) {
        *multikeyPaths = index.multikeyPaths;
    }
    return index.multikey;
}

bool CollectionImpl::setIndexIsMultikey(OperationContext* opCtx,
                                        StringData indexName,
                                        const MultikeyPaths& multikeyPaths,
                                        int indexOffset) const {

    int offset = indexOffset;
    if (offset < 0) {
        offset = _metadata->findIndexOffset(indexName);
        invariant(offset >= 0,
                  str::stream() << "cannot set multikey for index " << indexName << " @ "
                                << getCatalogId() << " : " << _metadata->toBSON());
    } else {
        invariant(offset < int(_metadata->indexes.size()),
                  str::stream() << "out of bounds index offset for multikey update" << indexName
                                << " @ " << getCatalogId() << " : " << _metadata->toBSON()
                                << "; offset : " << offset
                                << " ; actual : " << _metadata->findIndexOffset(indexName));
        invariant(indexName == _metadata->indexes[offset].nameStringData(),
                  str::stream() << "invalid index offset for multikey update " << indexName << " @ "
                                << getCatalogId() << " : " << _metadata->toBSON()
                                << "; offset : " << offset
                                << " ; actual : " << _metadata->findIndexOffset(indexName));
    }

    auto setMultikey = [offset,
                        multikeyPaths](const BSONCollectionCatalogEntry::MetaData& metadata) {
        auto* index = &metadata.indexes[offset];
        stdx::lock_guard lock(index->multikeyMutex);

        auto tracksPathLevelMultikeyInfo = !index->multikeyPaths.empty();
        if (!tracksPathLevelMultikeyInfo) {
            invariant(multikeyPaths.empty());

            if (index->multikey) {
                // The index is already set as multikey and we aren't tracking path-level
                // multikey information for it. We return false to indicate that the index
                // metadata is unchanged.
                return false;
            }
            index->multikey = true;
            return true;
        }

        // We are tracking path-level multikey information for this index.
        invariant(!multikeyPaths.empty());
        invariant(multikeyPaths.size() == index->multikeyPaths.size());

        index->multikey = true;

        bool newPathIsMultikey = false;
        bool somePathIsMultikey = false;

        // Store new path components that cause this index to be multikey in catalog's
        // index metadata.
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            auto& indexMultikeyComponents = index->multikeyPaths[i];
            for (const auto multikeyComponent : multikeyPaths[i]) {
                auto result = indexMultikeyComponents.insert(multikeyComponent);
                newPathIsMultikey = newPathIsMultikey || result.second;
                somePathIsMultikey = true;
            }
        }

        // If all of the sets in the multikey paths vector were empty, then no component
        // of any indexed field caused the index to be multikey. setIndexIsMultikey()
        // therefore shouldn't have been called.
        invariant(somePathIsMultikey);

        if (!newPathIsMultikey) {
            // We return false to indicate that the index metadata is unchanged.
            return false;
        }
        return true;
    };

    // Make a copy that is safe to read without locks that we insert in the durable catalog, we only
    // update the stored metadata on successful commit. The pending update is stored as a decoration
    // on the OperationContext to allow us to read our own writes.
    auto& uncommittedMultikeys = UncommittedMultikey::get(opCtx).resources();
    if (!uncommittedMultikeys) {
        uncommittedMultikeys = std::make_shared<UncommittedMultikey::MultikeyMap>();
    }
    BSONCollectionCatalogEntry::MetaData* metadata = nullptr;
    bool hasSetMultikey = false;

    if (auto it = uncommittedMultikeys->find(this); it != uncommittedMultikeys->end()) {
        metadata = &it->second;
        hasSetMultikey = setMultikey(*metadata);
    } else {
        // First time this OperationContext needs to change multikey information for this
        // collection. We cannot use the cached metadata in this collection as we may have just
        // committed a multikey change concurrently to the storage engine without being able to
        // observe it if its onCommit handlers haven't run yet.
        auto metadataLocal = *DurableCatalog::get(opCtx)->getMetaData(opCtx, getCatalogId());
        // When reading from the durable catalog the index offsets are different because when
        // removing indexes in-memory just zeros out the slot instead of actually removing it. We
        // must adjust the entries so they match how they are stored in _metadata so we can rely on
        // the index offsets being stable. The order of valid indexes are the same, so we can
        // iterate from the end and move them into the right positions.
        int localIdx = metadataLocal.indexes.size() - 1;
        metadataLocal.indexes.resize(_metadata->indexes.size());
        for (int i = _metadata->indexes.size() - 1; i >= 0 && localIdx != i; --i) {
            if (_metadata->indexes[i].isPresent()) {
                metadataLocal.indexes[i] = std::move(metadataLocal.indexes[localIdx]);
                metadataLocal.indexes[localIdx] = {};
                --localIdx;
            }
        }

        hasSetMultikey = setMultikey(metadataLocal);
        if (hasSetMultikey) {
            metadata = &uncommittedMultikeys->emplace(this, std::move(metadataLocal)).first->second;
        }
    }

    if (!hasSetMultikey)
        return false;

    opCtx->recoveryUnit()->onRollback(
        [this, uncommittedMultikeys](OperationContext*) { uncommittedMultikeys->erase(this); });

    DurableCatalog::get(opCtx)->putMetaData(opCtx, getCatalogId(), *metadata);

    // RAII Helper object to ensure we decrement the concurrent counter if and only if we
    // incremented it in a preCommit handler.
    class ConcurrentMultikeyWriteTracker {
    public:
        ConcurrentMultikeyWriteTracker(
            std::shared_ptr<const BSONCollectionCatalogEntry::MetaData> meta, int indexOffset)
            : metadata(std::move(meta)), offset(indexOffset) {}

        ~ConcurrentMultikeyWriteTracker() {
            if (hasIncremented) {
                metadata->indexes[offset].concurrentWriters.fetchAndSubtract(1);
            }
        }

        void preCommit() {
            metadata->indexes[offset].concurrentWriters.fetchAndAdd(1);
            hasIncremented = true;
        }

    private:
        std::shared_ptr<const BSONCollectionCatalogEntry::MetaData> metadata;
        int offset;
        bool hasIncremented = false;
    };

    auto concurrentWriteTracker =
        std::make_shared<ConcurrentMultikeyWriteTracker>(_metadata, offset);

    // Mark this index that there is an ongoing multikey write. This forces readers to read from the
    // durable catalog to determine if the index is multikey or not.
    opCtx->recoveryUnit()->registerPreCommitHook(
        [concurrentWriteTracker](OperationContext*) { concurrentWriteTracker->preCommit(); });

    // Capture a reference to 'concurrentWriteTracker' to extend the lifetime of this object until
    // commiting/rolling back the transaction is fully complete.
    opCtx->recoveryUnit()->onCommit(
        [this, uncommittedMultikeys, setMultikey = std::move(setMultikey), concurrentWriteTracker](
            OperationContext*, boost::optional<Timestamp>) {
            // Merge in changes to this index, other indexes may have been updated since we made our
            // copy. Don't check for result as another thread could be setting multikey at the same
            // time
            setMultikey(*_metadata);
            uncommittedMultikeys->erase(this);
        });

    return true;
}

void CollectionImpl::forceSetIndexIsMultikey(OperationContext* opCtx,
                                             const IndexDescriptor* desc,
                                             bool isMultikey,
                                             const MultikeyPaths& multikeyPaths) const {
    auto forceSetMultikey = [this,
                             isMultikey,
                             indexName = desc->indexName(),
                             accessMethod = desc->getAccessMethodName(),
                             numKeyPatternFields = desc->keyPattern().nFields(),
                             multikeyPaths](const BSONCollectionCatalogEntry::MetaData& metadata) {
        int offset = metadata.findIndexOffset(indexName);
        invariant(offset >= 0,
                  str::stream() << "cannot set index " << indexName << " multikey state @ "
                                << getCatalogId() << " : " << metadata.toBSON());

        const auto& index = metadata.indexes[offset];
        stdx::lock_guard lock(index.multikeyMutex);
        index.multikey = isMultikey;
        if (indexTypeSupportsPathLevelMultikeyTracking(accessMethod)) {
            if (isMultikey) {
                index.multikeyPaths = multikeyPaths;
            } else {
                index.multikeyPaths = MultikeyPaths{static_cast<size_t>(numKeyPatternFields)};
            }
        }
    };

    // Make a copy that is safe to read without locks that we insert in the durable catalog, we only
    // update the stored metadata on successful commit. The pending update is stored as a decoration
    // on the OperationContext to allow us to read our own writes.
    auto& uncommittedMultikeys = UncommittedMultikey::get(opCtx).resources();
    if (!uncommittedMultikeys) {
        uncommittedMultikeys = std::make_shared<UncommittedMultikey::MultikeyMap>();
    }
    BSONCollectionCatalogEntry::MetaData* metadata = nullptr;
    if (auto it = uncommittedMultikeys->find(this); it != uncommittedMultikeys->end()) {
        metadata = &it->second;
    } else {
        metadata = &uncommittedMultikeys->emplace(this, *_metadata).first->second;
    }
    forceSetMultikey(*metadata);

    opCtx->recoveryUnit()->onRollback(
        [this, uncommittedMultikeys](OperationContext*) { uncommittedMultikeys->erase(this); });

    DurableCatalog::get(opCtx)->putMetaData(opCtx, getCatalogId(), *metadata);

    opCtx->recoveryUnit()->onCommit(
        [this, uncommittedMultikeys, forceSetMultikey = std::move(forceSetMultikey)](
            OperationContext*, boost::optional<Timestamp>) {
            // Merge in changes to this index, other indexes may have been updated since we made our
            // copy.
            forceSetMultikey(*_metadata);
            uncommittedMultikeys->erase(this);
        });
}

int CollectionImpl::getTotalIndexCount() const {
    return _metadata->getTotalIndexCount();
}

int CollectionImpl::getCompletedIndexCount() const {
    int num = 0;
    for (unsigned i = 0; i < _metadata->indexes.size(); i++) {
        if (_metadata->indexes[i].ready)
            num++;
    }
    return num;
}

BSONObj CollectionImpl::getIndexSpec(StringData indexName) const {
    int offset = _metadata->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot get index spec for " << indexName << " @ " << getCatalogId()
                            << " : " << _metadata->toBSON());

    return _metadata->indexes[offset].spec;
}

void CollectionImpl::getAllIndexes(std::vector<std::string>* names) const {
    for (const auto& index : _metadata->indexes) {
        if (!index.isPresent()) {
            continue;
        }

        names->push_back(index.nameStringData().toString());
    }
}

void CollectionImpl::getReadyIndexes(std::vector<std::string>* names) const {
    for (unsigned i = 0; i < _metadata->indexes.size(); i++) {
        if (_metadata->indexes[i].ready)
            names->push_back(_metadata->indexes[i].spec["name"].String());
    }
}

bool CollectionImpl::isIndexPresent(StringData indexName) const {
    int offset = _metadata->findIndexOffset(indexName);
    return offset >= 0;
}

bool CollectionImpl::isIndexReady(StringData indexName) const {
    int offset = _metadata->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot get ready status for index " << indexName << " @ "
                            << getCatalogId() << " : " << _metadata->toBSON());
    return _metadata->indexes[offset].ready;
}

void CollectionImpl::replaceMetadata(OperationContext* opCtx,
                                     std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md) {
    DurableCatalog::get(opCtx)->putMetaData(opCtx, getCatalogId(), *md);
    _metadata = std::move(md);
}

bool CollectionImpl::isMetadataEqual(const BSONObj& otherMetadata) const {
    return !_metadata->toBSON().woCompare(otherMetadata);
}

template <typename Func>
void CollectionImpl::_writeMetadata(OperationContext* opCtx, Func func) {
    // Even though we are holding an exclusive lock on the Collection there may be an ongoing
    // multikey change on this OperationContext. Make sure we include that update when we copy the
    // metadata for this operation.
    const BSONCollectionCatalogEntry::MetaData* sourceMetadata = _metadata.get();
    auto& uncommittedMultikeys = UncommittedMultikey::get(opCtx).resources();
    if (uncommittedMultikeys) {
        if (auto it = uncommittedMultikeys->find(this); it != uncommittedMultikeys->end()) {
            sourceMetadata = &it->second;
        }
    }

    // Copy metadata and apply provided function to make change.
    auto metadata = std::make_shared<BSONCollectionCatalogEntry::MetaData>(*sourceMetadata);
    func(*metadata);

    // Remove the cached multikey change, it is now included in the copied metadata. If we left it
    // here we could read stale data.
    if (uncommittedMultikeys) {
        uncommittedMultikeys->erase(this);
    }

    // Store in durable catalog and replace pointer with our copied instance.
    DurableCatalog::get(opCtx)->putMetaData(opCtx, getCatalogId(), *metadata);
    _metadata = std::move(metadata);
}

}  // namespace mongo
