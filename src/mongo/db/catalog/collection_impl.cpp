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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/private/record_store_validate_adaptor.h"

#include <fmt/format.h>

#include "mongo/db/catalog/collection_impl.h"

#include "mongo/base/counter.h"
#include "mongo/base/init.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_info_cache_impl.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog_impl.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/update/update_driver.h"

#include "mongo/db/auth/user_document_parser.h"  // XXX-ANDY
#include "mongo/rpc/object_check.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
//  This fail point injects insertion failures for all collections unless a collection name is
//  provided in the optional data object during configuration:
//  data: {
//      collectionNS: <fully-qualified collection namespace>,
//  }
MONGO_FAIL_POINT_DEFINE(failCollectionInserts);

// Used to pause after inserting collection data and calling the opObservers.  Inserts to
// replicated collections that are not part of a multi-statement transaction will have generated
// their OpTime and oplog entry. Supports parameters to limit pause by namespace and by _id
// of first data item in an insert (must be of type string):
//  data: {
//      collectionNS: <fully-qualified collection namespace>,
//      first_id: <string>
//  }
MONGO_FAIL_POINT_DEFINE(hangAfterCollectionInserts);

// This fail point throws a WriteConflictException after a successful call to insertRecords.
MONGO_FAIL_POINT_DEFINE(failAfterBulkLoadDocInsert);

// This fail point allows collections to be given malformed validator. A malformed validator
// will not (and cannot) be enforced but it will be persisted.
MONGO_FAIL_POINT_DEFINE(allowSettingMalformedCollectionValidators);

/**
 * Checks the 'failCollectionInserts' fail point at the beginning of an insert operation to see if
 * the insert should fail. Returns Status::OK if The function should proceed with the insertion.
 * Otherwise, the function should fail and return early with the error Status.
 */
Status checkFailCollectionInsertsFailPoint(const NamespaceString& ns, const BSONObj& firstDoc) {
    MONGO_FAIL_POINT_BLOCK(failCollectionInserts, extraData) {
        const BSONObj& data = extraData.getData();
        const auto collElem = data["collectionNS"];
        // If the failpoint specifies no collection or matches the existing one, fail.
        if (!collElem || ns.ns() == collElem.str()) {
            const std::string msg = str::stream()
                << "Failpoint (failCollectionInserts) has been enabled (" << data
                << "), so rejecting insert (first doc): " << firstDoc;
            log() << msg;
            return {ErrorCodes::FailPointEnabled, msg};
        }
    }
    return Status::OK();
}

// Uses the collator factory to convert the BSON representation of a collator to a
// CollatorInterface. Returns null if the BSONObj is empty. We expect the stored collation to be
// valid, since it gets validated on collection create.
std::unique_ptr<CollatorInterface> parseCollation(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  BSONObj collationSpec) {
    if (collationSpec.isEmpty()) {
        return {nullptr};
    }

    auto collator =
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationSpec);

    // If the collection's default collator has a version not currently supported by our ICU
    // integration, shut down the server. Errors other than IncompatibleCollationVersion should not
    // be possible, so these are an invariant rather than fassert.
    if (collator == ErrorCodes::IncompatibleCollationVersion) {
        log() << "Collection " << nss
              << " has a default collation which is incompatible with this version: "
              << collationSpec;
        fassertFailedNoTrace(40144);
    }
    invariant(collator.getStatus());

    return std::move(collator.getValue());
}

StatusWith<CollectionImpl::ValidationLevel> _parseValidationLevel(StringData newLevel) {
    auto status = Collection::parseValidationLevel(newLevel);
    if (!status.isOK()) {
        return status;
    }

    if (newLevel == "") {
        // default
        return CollectionImpl::ValidationLevel::STRICT_V;
    } else if (newLevel == "off") {
        return CollectionImpl::ValidationLevel::OFF;
    } else if (newLevel == "moderate") {
        return CollectionImpl::ValidationLevel::MODERATE;
    } else if (newLevel == "strict") {
        return CollectionImpl::ValidationLevel::STRICT_V;
    }

    MONGO_UNREACHABLE;
}

StatusWith<CollectionImpl::ValidationAction> _parseValidationAction(StringData newAction) {
    auto status = Collection::parseValidationAction(newAction);
    if (!status.isOK()) {
        return status;
    }

    if (newAction == "") {
        // default
        return CollectionImpl::ValidationAction::ERROR_V;
    } else if (newAction == "warn") {
        return CollectionImpl::ValidationAction::WARN;
    } else if (newAction == "error") {
        return CollectionImpl::ValidationAction::ERROR_V;
    }

    MONGO_UNREACHABLE;
}

Status checkValidatorCanBeUsedOnNs(const BSONObj& validator,
                                   const NamespaceString& nss,
                                   const OptionalCollectionUUID& uuid) {
    if (validator.isEmpty())
        return Status::OK();

    if (nss.isSystem() && !nss.isDropPendingNamespace()) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Document validators not allowed on system collection " << nss
                              << " with UUID " << (uuid ? " with UUID " + uuid->toString() : "")};
    }

    if (nss.isOnInternalDb()) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Document validators are not allowed on collection " << nss.ns()
                              << " with UUID " << (uuid ? " with UUID " + uuid->toString() : "")
                              << " in the " << nss.db() << " internal database"};
    }
    return Status::OK();
}

}  // namespace

using std::string;
using std::unique_ptr;
using std::vector;

using logger::LogComponent;

CollectionImpl::CollectionImpl(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               std::unique_ptr<RecordStore> recordStore)
    : _magic(kMagicNumber),
      _ns(nss),
      _uuid(uuid),
      _recordStore(std::move(recordStore)),
      _needCappedLock(supportsDocLocking() && _recordStore && _recordStore->isCapped() &&
                      _ns.db() != "local"),
      _infoCache(std::make_unique<CollectionInfoCacheImpl>(this, _ns)),
      _indexCatalog(std::make_unique<IndexCatalogImpl>(this)),
      _swValidator{nullptr},
      _cappedNotifier(_recordStore && _recordStore->isCapped()
                          ? stdx::make_unique<CappedInsertNotifier>()
                          : nullptr) {
    if (isCapped())
        _recordStore->setCappedCallback(this);
}

CollectionImpl::~CollectionImpl() {
    invariant(ok());
    if (isCapped()) {
        _recordStore->setCappedCallback(nullptr);
        _cappedNotifier->kill();
    }

    if (ns().isOplog()) {
        repl::clearLocalOplogPtr();
    }

    _magic = 0;
}

std::unique_ptr<Collection> CollectionImpl::FactoryImpl::make(
    OperationContext* opCtx,
    const NamespaceString& nss,
    CollectionUUID uuid,
    std::unique_ptr<RecordStore> rs) const {
    return std::make_unique<CollectionImpl>(opCtx, nss, uuid, std::move(rs));
}

void CollectionImpl::init(OperationContext* opCtx) {
    auto collectionOptions = DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, _ns);
    _collator = parseCollation(opCtx, _ns, collectionOptions.collation);
    _validatorDoc = collectionOptions.validator.getOwned();

    // Enforce that the validator can be used on this namespace.
    uassertStatusOK(checkValidatorCanBeUsedOnNs(_validatorDoc, ns(), _uuid));

    // Make sure to parse the action and level before the MatchExpression, since certain features
    // are not supported with certain combinations of action and level.
    _validationAction = uassertStatusOK(_parseValidationAction(collectionOptions.validationAction));
    _validationLevel = uassertStatusOK(_parseValidationLevel(collectionOptions.validationLevel));

    // Store the result (OK / error) of parsing the validator, but do not enforce that the result is
    // OK. This is intentional, as users may have validators on disk which were considered well
    // formed in older versions but not in newer versions.
    _swValidator =
        parseValidator(opCtx, _validatorDoc, MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!_swValidator.isOK()) {
        // Log an error and startup warning if the collection validator is malformed.
        warning() << "Collection " << _ns
                  << " has malformed validator: " << _swValidator.getStatus() << startupWarningsLog;
    }

    getIndexCatalog()->init(opCtx).transitional_ignore();
    infoCache()->init(opCtx);
    _initialized = true;
}

bool CollectionImpl::isInitialized() const {
    return _initialized;
}

bool CollectionImpl::requiresIdIndex() const {
    if (_ns.isVirtualized() || _ns.isOplog()) {
        // No indexes on virtual collections or the oplog.
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
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_IS));
    invariant(ok());

    return _recordStore->getCursor(opCtx, forward);
}


bool CollectionImpl::findDoc(OperationContext* opCtx,
                             RecordId loc,
                             Snapshotted<BSONObj>* out) const {
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_IS));

    RecordData rd;
    if (!_recordStore->findRecord(opCtx, loc, &rd))
        return false;
    *out = Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(), rd.releaseToBson());
    return true;
}

Status CollectionImpl::checkValidation(OperationContext* opCtx, const BSONObj& document) const {
    if (!_swValidator.isOK()) {
        return _swValidator.getStatus();
    }

    const auto* const validatorMatchExpr = _swValidator.getValue().get();
    if (!validatorMatchExpr)
        return Status::OK();

    if (_validationLevel == ValidationLevel::OFF)
        return Status::OK();

    if (documentValidationDisabled(opCtx))
        return Status::OK();

    if (validatorMatchExpr->matchesBSON(document))
        return Status::OK();

    if (_validationAction == ValidationAction::WARN) {
        warning() << "Document would fail validation"
                  << " collection: " << ns() << " doc: " << redact(document);
        return Status::OK();
    }

    return {ErrorCodes::DocumentValidationFailure, "Document failed validation"};
}

StatusWithMatchExpression CollectionImpl::parseValidator(
    OperationContext* opCtx,
    const BSONObj& validator,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
        maxFeatureCompatibilityVersion) const {
    if (MONGO_unlikely(allowSettingMalformedCollectionValidators.shouldFail())) {
        return {nullptr};
    }

    if (validator.isEmpty())
        return {nullptr};

    Status canUseValidatorInThisContext = checkValidatorCanBeUsedOnNs(validator, ns(), _uuid);
    if (!canUseValidatorInThisContext.isOK()) {
        return canUseValidatorInThisContext;
    }

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, _collator.get()));

    // The MatchExpression and contained ExpressionContext created as part of the validator are
    // owned by the Collection and will outlive the OperationContext they were created under.
    expCtx->opCtx = nullptr;

    // Enforce a maximum feature version if requested.
    expCtx->maxFeatureCompatibilityVersion = maxFeatureCompatibilityVersion;

    // If the validation action is "warn" or the level is "moderate", then disallow any encryption
    // keywords. This is to prevent any plaintext data from showing up in the logs.
    if (_validationAction == CollectionImpl::ValidationAction::WARN ||
        _validationLevel == CollectionImpl::ValidationLevel::MODERATE)
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

    auto statusWithMatcher =
        MatchExpressionParser::parse(validator, expCtx, ExtensionsCallbackNoop(), allowedFeatures);

    if (!statusWithMatcher.isOK()) {
        return StatusWithMatchExpression{
            statusWithMatcher.getStatus().withContext("Parsing of collection validator failed")};
    }

    return statusWithMatcher;
}

Status CollectionImpl::insertDocumentsForOplog(OperationContext* opCtx,
                                               const DocWriter* const* docs,
                                               Timestamp* timestamps,
                                               size_t nDocs) {
    dassert(opCtx->lockState()->isWriteLocked());

    // Since this is only for the OpLog, we can assume these for simplicity.
    // This also means that we do not need to forward this object to the OpObserver, which is good
    // because it would defeat the purpose of using DocWriter.
    invariant(_swValidator.isOK());
    invariant(_swValidator.getValue() == nullptr);
    invariant(!_indexCatalog->haveAnyIndexes());

    Status status = _recordStore->insertRecordsWithDocWriter(opCtx, docs, timestamps, nDocs);
    if (!status.isOK())
        return status;

    opCtx->recoveryUnit()->onCommit(
        [this](boost::optional<Timestamp>) { notifyCappedWaitersIfNeeded(); });

    return status;
}


Status CollectionImpl::insertDocuments(OperationContext* opCtx,
                                       const vector<InsertStatement>::const_iterator begin,
                                       const vector<InsertStatement>::const_iterator end,
                                       OpDebug* opDebug,
                                       bool fromMigrate) {

    auto status = checkFailCollectionInsertsFailPoint(_ns, (begin != end ? begin->doc : BSONObj()));
    if (!status.isOK()) {
        return status;
    }

    // Should really be done in the collection object at creation and updated on index create.
    const bool hasIdIndex = _indexCatalog->findIdIndex(opCtx);

    for (auto it = begin; it != end; it++) {
        if (hasIdIndex && it->doc["_id"].eoo()) {
            return Status(ErrorCodes::InternalError,
                          str::stream()
                              << "Collection::insertDocument got document without _id for ns:"
                              << _ns);
        }

        auto status = checkValidation(opCtx, it->doc);
        if (!status.isOK())
            return status;
    }

    const SnapshotId sid = opCtx->recoveryUnit()->getSnapshotId();

    status = _insertDocuments(opCtx, begin, end, opDebug);
    if (!status.isOK()) {
        return status;
    }
    invariant(sid == opCtx->recoveryUnit()->getSnapshotId());

    getGlobalServiceContext()->getOpObserver()->onInserts(
        opCtx, ns(), uuid(), begin, end, fromMigrate);

    opCtx->recoveryUnit()->onCommit(
        [this](boost::optional<Timestamp>) { notifyCappedWaitersIfNeeded(); });

    MONGO_FAIL_POINT_BLOCK(hangAfterCollectionInserts, extraData) {
        const BSONObj& data = extraData.getData();
        const auto collElem = data["collectionNS"];
        const auto firstIdElem = data["first_id"];
        // If the failpoint specifies no collection or matches the existing one, hang.
        if ((!collElem || _ns.ns() == collElem.str()) &&
            (!firstIdElem ||
             (begin != end && firstIdElem.type() == mongo::String &&
              begin->doc["_id"].str() == firstIdElem.str()))) {
            string whenFirst =
                firstIdElem ? (string(" when first _id is ") + firstIdElem.str()) : "";
            while (MONGO_FAIL_POINT(hangAfterCollectionInserts)) {
                log() << "hangAfterCollectionInserts fail point enabled for " << _ns << whenFirst
                      << ". Blocking until fail point is disabled.";
                mongo::sleepsecs(1);
                opCtx->checkForInterrupt();
            }
        }
    }

    return Status::OK();
}

Status CollectionImpl::insertDocument(OperationContext* opCtx,
                                      const InsertStatement& docToInsert,
                                      OpDebug* opDebug,
                                      bool fromMigrate) {
    vector<InsertStatement> docs;
    docs.push_back(docToInsert);
    return insertDocuments(opCtx, docs.begin(), docs.end(), opDebug, fromMigrate);
}

Status CollectionImpl::insertDocumentForBulkLoader(OperationContext* opCtx,
                                                   const BSONObj& doc,
                                                   const OnRecordInsertedFn& onRecordInserted) {

    auto status = checkFailCollectionInsertsFailPoint(_ns, doc);
    if (!status.isOK()) {
        return status;
    }

    status = checkValidation(opCtx, doc);
    if (!status.isOK()) {
        return status;
    }

    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_IX));

    // TODO SERVER-30638: using timestamp 0 for these inserts, which are non-oplog so we don't yet
    // care about their correct timestamps.
    StatusWith<RecordId> loc =
        _recordStore->insertRecord(opCtx, doc.objdata(), doc.objsize(), Timestamp());

    if (!loc.isOK())
        return loc.getStatus();

    status = onRecordInserted(loc.getValue());

    if (MONGO_FAIL_POINT(failAfterBulkLoadDocInsert)) {
        log() << "Failpoint failAfterBulkLoadDocInsert enabled for " << _ns.ns()
              << ". Throwing WriteConflictException.";
        throw WriteConflictException();
    }

    vector<InsertStatement> inserts;
    OplogSlot slot;
    // Fetch a new optime now, if necessary.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->isOplogDisabledFor(opCtx, _ns)) {
        // Populate 'slot' with a new optime.
        slot = repl::getNextOpTime(opCtx);
    }
    inserts.emplace_back(kUninitializedStmtId, doc, slot);

    getGlobalServiceContext()->getOpObserver()->onInserts(
        opCtx, ns(), uuid(), inserts.begin(), inserts.end(), false);

    opCtx->recoveryUnit()->onCommit(
        [this](boost::optional<Timestamp>) { notifyCappedWaitersIfNeeded(); });

    return loc.getStatus();
}

Status CollectionImpl::_insertDocuments(OperationContext* opCtx,
                                        const vector<InsertStatement>::const_iterator begin,
                                        const vector<InsertStatement>::const_iterator end,
                                        OpDebug* opDebug) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_IX));

    const size_t count = std::distance(begin, end);
    if (isCapped() && _indexCatalog->haveAnyIndexes() && count > 1) {
        // We require that inserts to indexed capped collections be done one-at-a-time to avoid the
        // possibility that a later document causes an earlier document to be deleted before it can
        // be indexed.
        // TODO SERVER-21512 It would be better to handle this here by just doing single inserts.
        return {ErrorCodes::OperationCannotBeBatched,
                "Can't batch inserts into indexed capped collections"};
    }

    if (_needCappedLock) {
        // X-lock the metadata resource for this capped collection until the end of the WUOW. This
        // prevents the primary from executing with more concurrency than secondaries.
        // See SERVER-21646.
        Lock::ResourceLock heldUntilEndOfWUOW{
            opCtx->lockState(), ResourceId(RESOURCE_METADATA, _ns.ns()), MODE_X};
    }

    std::vector<Record> records;
    records.reserve(count);
    std::vector<Timestamp> timestamps;
    timestamps.reserve(count);

    for (auto it = begin; it != end; it++) {
        records.emplace_back(Record{RecordId(), RecordData(it->doc.objdata(), it->doc.objsize())});
        timestamps.emplace_back(it->oplogSlot.getTimestamp());
    }
    Status status = _recordStore->insertRecords(opCtx, &records, timestamps);
    if (!status.isOK())
        return status;

    std::vector<BsonRecord> bsonRecords;
    bsonRecords.reserve(count);
    int recordIndex = 0;
    for (auto it = begin; it != end; it++) {
        RecordId loc = records[recordIndex++].id;
        invariant(RecordId::min() < loc);
        invariant(loc < RecordId::max());

        BsonRecord bsonRecord = {loc, Timestamp(it->oplogSlot.getTimestamp()), &(it->doc)};
        bsonRecords.push_back(bsonRecord);
    }

    int64_t keysInserted;
    status = _indexCatalog->indexRecords(opCtx, bsonRecords, &keysInserted);
    if (opDebug) {
        opDebug->additiveMetrics.incrementKeysInserted(keysInserted);
    }

    return status;
}

void CollectionImpl::setMinimumVisibleSnapshot(Timestamp newMinimumVisibleSnapshot) {
    if (!_minVisibleSnapshot || (newMinimumVisibleSnapshot > _minVisibleSnapshot.get())) {
        _minVisibleSnapshot = newMinimumVisibleSnapshot;
    }
}

bool CollectionImpl::haveCappedWaiters() {
    // Waiters keep a shared_ptr to '_cappedNotifier', so there are waiters if this CollectionImpl's
    // shared_ptr is not unique (use_count > 1).
    return _cappedNotifier.use_count() > 1;
}

void CollectionImpl::notifyCappedWaitersIfNeeded() {
    // If there is a notifier object and another thread is waiting on it, then we notify
    // waiters of this document insert.
    if (haveCappedWaiters())
        _cappedNotifier->notifyAll();
}

Status CollectionImpl::aboutToDeleteCapped(OperationContext* opCtx,
                                           const RecordId& loc,
                                           RecordData data) {
    BSONObj doc = data.releaseToBson();
    int64_t* const nullKeysDeleted = nullptr;
    _indexCatalog->unindexRecord(opCtx, doc, loc, false, nullKeysDeleted);

    // We are not capturing and reporting to OpDebug the 'keysDeleted' by unindexRecord(). It is
    // questionable whether reporting will add diagnostic value to users and may instead be
    // confusing as it depends on our internal capped collection document removal strategy.
    // We can consider adding either keysDeleted or a new metric reporting document removal if
    // justified by user demand.

    return Status::OK();
}

void CollectionImpl::deleteDocument(OperationContext* opCtx,
                                    StmtId stmtId,
                                    RecordId loc,
                                    OpDebug* opDebug,
                                    bool fromMigrate,
                                    bool noWarn,
                                    Collection::StoreDeletedDoc storeDeletedDoc) {
    if (isCapped()) {
        log() << "failing remove on a capped ns " << _ns;
        uasserted(10089, "cannot remove from a capped collection");
        return;
    }

    Snapshotted<BSONObj> doc = docFor(opCtx, loc);
    getGlobalServiceContext()->getOpObserver()->aboutToDelete(opCtx, ns(), doc.value());

    boost::optional<BSONObj> deletedDoc;
    if (storeDeletedDoc == Collection::StoreDeletedDoc::On) {
        deletedDoc.emplace(doc.value().getOwned());
    }

    int64_t keysDeleted;
    _indexCatalog->unindexRecord(opCtx, doc.value(), loc, noWarn, &keysDeleted);
    _recordStore->deleteRecord(opCtx, loc);

    OpObserver::OplogDeleteEntryArgs deleteArgs;
    if (deletedDoc) {
        deleteArgs.deletedDoc = &(deletedDoc.get());
    }
    deleteArgs.fromMigrate = fromMigrate;

    getGlobalServiceContext()->getOpObserver()->onDelete(opCtx, ns(), uuid(), stmtId, deleteArgs);

    if (opDebug) {
        opDebug->additiveMetrics.incrementKeysDeleted(keysDeleted);
    }
}

Counter64 moveCounter;
ServerStatusMetricField<Counter64> moveCounterDisplay("record.moves", &moveCounter);

RecordId CollectionImpl::updateDocument(OperationContext* opCtx,
                                        RecordId oldLocation,
                                        const Snapshotted<BSONObj>& oldDoc,
                                        const BSONObj& newDoc,
                                        bool indexesAffected,
                                        OpDebug* opDebug,
                                        CollectionUpdateArgs* args) {
    {
        auto status = checkValidation(opCtx, newDoc);
        if (!status.isOK()) {
            if (_validationLevel == ValidationLevel::STRICT_V) {
                uassertStatusOK(status);
            }
            // moderate means we have to check the old doc
            auto oldDocStatus = checkValidation(opCtx, oldDoc.value());
            if (oldDocStatus.isOK()) {
                // transitioning from good -> bad is not ok
                uassertStatusOK(status);
            }
            // bad -> bad is ok in moderate mode
        }
    }

    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_IX));
    invariant(oldDoc.snapshotId() == opCtx->recoveryUnit()->getSnapshotId());
    invariant(newDoc.isOwned());

    if (_needCappedLock) {
        // X-lock the metadata resource for this capped collection until the end of the WUOW. This
        // prevents the primary from executing with more concurrency than secondaries.
        // See SERVER-21646.
        Lock::ResourceLock heldUntilEndOfWUOW{
            opCtx->lockState(), ResourceId(RESOURCE_METADATA, _ns.ns()), MODE_X};
    }

    SnapshotId sid = opCtx->recoveryUnit()->getSnapshotId();

    BSONElement oldId = oldDoc.value()["_id"];
    if (!oldId.eoo() && SimpleBSONElementComparator::kInstance.evaluate(oldId != newDoc["_id"]))
        uasserted(13596, "in Collection::updateDocument _id mismatch");

    // The MMAPv1 storage engine implements capped collections in a way that does not allow records
    // to grow beyond their original size. If MMAPv1 part of a replicaset with storage engines that
    // do not have this limitation, replication could result in errors, so it is necessary to set a
    // uniform rule here. Similarly, it is not sufficient to disallow growing records, because this
    // happens when secondaries roll back an update shrunk a record. Exactly replicating legacy
    // MMAPv1 behavior would require padding shrunk documents on all storage engines. Instead forbid
    // all size changes.
    const auto oldSize = oldDoc.value().objsize();
    if (_recordStore->isCapped() && oldSize != newDoc.objsize())
        uasserted(ErrorCodes::CannotGrowDocumentInCappedNamespace,
                  str::stream() << "Cannot change the size of a document in a capped collection: "
                                << oldSize << " != " << newDoc.objsize());

    args->preImageDoc = oldDoc.value().getOwned();

    uassertStatusOK(
        _recordStore->updateRecord(opCtx, oldLocation, newDoc.objdata(), newDoc.objsize()));

    if (indexesAffected) {
        int64_t keysInserted, keysDeleted;

        uassertStatusOK(_indexCatalog->updateRecord(
            opCtx, args->preImageDoc.get(), newDoc, oldLocation, &keysInserted, &keysDeleted));

        if (opDebug) {
            opDebug->additiveMetrics.incrementKeysInserted(keysInserted);
            opDebug->additiveMetrics.incrementKeysDeleted(keysDeleted);
        }
    }

    invariant(sid == opCtx->recoveryUnit()->getSnapshotId());
    args->updatedDoc = newDoc;

    invariant(uuid());
    OplogUpdateEntryArgs entryArgs(*args, ns(), *uuid());
    getGlobalServiceContext()->getOpObserver()->onUpdate(opCtx, entryArgs);

    return {oldLocation};
}

bool CollectionImpl::updateWithDamagesSupported() const {
    if (!_swValidator.isOK() || _swValidator.getValue() != nullptr)
        return false;

    return _recordStore->updateWithDamagesSupported();
}

StatusWith<RecordData> CollectionImpl::updateDocumentWithDamages(
    OperationContext* opCtx,
    RecordId loc,
    const Snapshotted<RecordData>& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages,
    CollectionUpdateArgs* args) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_IX));
    invariant(oldRec.snapshotId() == opCtx->recoveryUnit()->getSnapshotId());
    invariant(updateWithDamagesSupported());

    auto newRecStatus =
        _recordStore->updateWithDamages(opCtx, loc, oldRec.value(), damageSource, damages);

    if (newRecStatus.isOK()) {
        args->updatedDoc = newRecStatus.getValue().toBson();

        invariant(uuid());
        OplogUpdateEntryArgs entryArgs(*args, ns(), *uuid());
        getGlobalServiceContext()->getOpObserver()->onUpdate(opCtx, entryArgs);
    }
    return newRecStatus;
}

bool CollectionImpl::isTemporary(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, _ns).temp;
}

bool CollectionImpl::isCapped() const {
    return _cappedNotifier.get();
}

CappedCallback* CollectionImpl::getCappedCallback() {
    return this;
}

std::shared_ptr<CappedInsertNotifier> CollectionImpl::getCappedInsertNotifier() const {
    invariant(isCapped());
    return _cappedNotifier;
}

uint64_t CollectionImpl::numRecords(OperationContext* opCtx) const {
    return _recordStore->numRecords(opCtx);
}

uint64_t CollectionImpl::dataSize(OperationContext* opCtx) const {
    return _recordStore->dataSize(opCtx);
}

uint64_t CollectionImpl::getIndexSize(OperationContext* opCtx,
                                      BSONObjBuilder* details,
                                      int scale) const {
    const IndexCatalog* idxCatalog = getIndexCatalog();

    std::unique_ptr<IndexCatalog::IndexIterator> ii = idxCatalog->getIndexIterator(opCtx, true);

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

/**
 * order will be:
 * 1) store index specs
 * 2) drop indexes
 * 3) truncate record store
 * 4) re-write indexes
 */
Status CollectionImpl::truncate(OperationContext* opCtx) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));
    invariant(_indexCatalog->numIndexesInProgress(opCtx) == 0);

    // 1) store index specs
    vector<BSONObj> indexSpecs;
    {
        std::unique_ptr<IndexCatalog::IndexIterator> ii =
            _indexCatalog->getIndexIterator(opCtx, false);
        while (ii->more()) {
            const IndexDescriptor* idx = ii->next()->descriptor();
            indexSpecs.push_back(idx->infoObj().getOwned());
        }
    }

    // 2) drop indexes
    _indexCatalog->dropAllIndexes(opCtx, true);

    // 3) truncate record store
    auto status = _recordStore->truncate(opCtx);
    if (!status.isOK())
        return status;

    // 4) re-create indexes
    for (size_t i = 0; i < indexSpecs.size(); i++) {
        status = _indexCatalog->createIndexOnEmptyCollection(opCtx, indexSpecs[i]).getStatus();
        if (!status.isOK())
            return status;
    }

    return Status::OK();
}

void CollectionImpl::cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));
    invariant(isCapped());
    invariant(_indexCatalog->numIndexesInProgress(opCtx) == 0);

    _recordStore->cappedTruncateAfter(opCtx, end, inclusive);
}

Status CollectionImpl::setValidator(OperationContext* opCtx, BSONObj validatorDoc) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    // Make owned early so that the parsed match expression refers to the owned object.
    if (!validatorDoc.isOwned())
        validatorDoc = validatorDoc.getOwned();

    // Note that, by the time we reach this, we should have already done a pre-parse that checks for
    // banned features, so we don't need to include that check again.
    auto statusWithMatcher =
        parseValidator(opCtx, validatorDoc, MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!statusWithMatcher.isOK())
        return statusWithMatcher.getStatus();

    DurableCatalog::get(opCtx)->updateValidator(
        opCtx, ns(), validatorDoc, getValidationLevel(), getValidationAction());

    opCtx->recoveryUnit()->onRollback([this,
                                       oldValidator = std::move(_swValidator),
                                       oldValidatorDoc = std::move(_validatorDoc)]() mutable {
        this->_swValidator = std::move(oldValidator);
        this->_validatorDoc = std::move(oldValidatorDoc);
    });
    _swValidator = std::move(statusWithMatcher);
    _validatorDoc = std::move(validatorDoc);
    return Status::OK();
}

StringData CollectionImpl::getValidationLevel() const {
    switch (_validationLevel) {
        case ValidationLevel::STRICT_V:
            return "strict";
        case ValidationLevel::OFF:
            return "off";
        case ValidationLevel::MODERATE:
            return "moderate";
    }
    MONGO_UNREACHABLE;
}

StringData CollectionImpl::getValidationAction() const {
    switch (_validationAction) {
        case ValidationAction::ERROR_V:
            return "error";
        case ValidationAction::WARN:
            return "warn";
    }
    MONGO_UNREACHABLE;
}

Status CollectionImpl::setValidationLevel(OperationContext* opCtx, StringData newLevel) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    auto levelSW = _parseValidationLevel(newLevel);
    if (!levelSW.isOK()) {
        return levelSW.getStatus();
    }

    auto oldValidationLevel = _validationLevel;
    _validationLevel = levelSW.getValue();

    opCtx->recoveryUnit()->onRollback(
        [this, oldValidationLevel]() { this->_validationLevel = oldValidationLevel; });

    // Reparse the validator as there are some features which are only supported with certain
    // validation levels.
    auto allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures;
    if (_validationLevel == CollectionImpl::ValidationLevel::MODERATE)
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

    _swValidator = parseValidator(opCtx, _validatorDoc, allowedFeatures);
    if (!_swValidator.isOK()) {
        return _swValidator.getStatus();
    }

    DurableCatalog::get(opCtx)->updateValidator(
        opCtx, ns(), _validatorDoc, getValidationLevel(), getValidationAction());

    return Status::OK();
}

Status CollectionImpl::setValidationAction(OperationContext* opCtx, StringData newAction) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    auto actionSW = _parseValidationAction(newAction);
    if (!actionSW.isOK()) {
        return actionSW.getStatus();
    }

    auto oldValidationAction = _validationAction;
    _validationAction = actionSW.getValue();

    opCtx->recoveryUnit()->onRollback(
        [this, oldValidationAction]() { this->_validationAction = oldValidationAction; });

    // Reparse the validator as there are some features which are only supported with certain
    // validation actions.
    auto allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures;
    if (_validationAction == CollectionImpl::ValidationAction::WARN)
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

    _swValidator = parseValidator(opCtx, _validatorDoc, allowedFeatures);
    if (!_swValidator.isOK()) {
        return _swValidator.getStatus();
    }

    DurableCatalog::get(opCtx)->updateValidator(
        opCtx, ns(), _validatorDoc, getValidationLevel(), getValidationAction());

    return Status::OK();
}

Status CollectionImpl::updateValidator(OperationContext* opCtx,
                                       BSONObj newValidator,
                                       StringData newLevel,
                                       StringData newAction) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_X));

    opCtx->recoveryUnit()->onRollback([this,
                                       oldValidator = std::move(_swValidator),
                                       oldValidatorDoc = std::move(_validatorDoc),
                                       oldValidationLevel = _validationLevel,
                                       oldValidationAction = _validationAction]() mutable {
        this->_swValidator = std::move(oldValidator);
        this->_validatorDoc = std::move(oldValidatorDoc);
        this->_validationLevel = oldValidationLevel;
        this->_validationAction = oldValidationAction;
    });

    DurableCatalog::get(opCtx)->updateValidator(opCtx, ns(), newValidator, newLevel, newAction);
    _validatorDoc = std::move(newValidator);

    auto validatorSW =
        parseValidator(opCtx, _validatorDoc, MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!validatorSW.isOK()) {
        return validatorSW.getStatus();
    }
    _swValidator = std::move(validatorSW.getValue());

    auto levelSW = _parseValidationLevel(newLevel);
    if (!levelSW.isOK()) {
        return levelSW.getStatus();
    }
    _validationLevel = levelSW.getValue();

    auto actionSW = _parseValidationAction(newAction);
    if (!actionSW.isOK()) {
        return actionSW.getStatus();
    }
    _validationAction = actionSW.getValue();

    return Status::OK();
}

const CollatorInterface* CollectionImpl::getDefaultCollator() const {
    return _collator.get();
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
        const auto& newIndexSpec = validateResult.getValue();

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

        newIndexSpecs.push_back(newIndexSpec);
    }

    return newIndexSpecs;
}

namespace {

using ValidateResultsMap = std::map<std::string, ValidateResults>;

// General validation logic for any RecordStore. Performs sanity checks to confirm that each
// record in the store is valid according to the given RecordStoreValidateAdaptor and updates
// record store stats to match.
void _genericRecordStoreValidate(OperationContext* opCtx,
                                 RecordStore* recordStore,
                                 RecordStoreValidateAdaptor* indexValidator,
                                 ValidateResults* results,
                                 BSONObjBuilder* output) {
    long long nrecords = 0;
    long long dataSizeTotal = 0;
    long long nInvalid = 0;

    results->valid = true;
    std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx, true);
    int interruptInterval = 4096;
    RecordId prevRecordId;

    while (auto record = cursor->next()) {
        if (!(nrecords % interruptInterval)) {
            opCtx->checkForInterrupt();
        }
        ++nrecords;
        auto dataSize = record->data.size();
        dataSizeTotal += dataSize;
        size_t validatedSize;
        Status status = indexValidator->validate(record->id, record->data, &validatedSize);

        // Check to ensure isInRecordIdOrder() is being used properly.
        if (prevRecordId.isValid()) {
            invariant(prevRecordId < record->id);
        }

        // ValidatedSize = dataSize is not a general requirement as some storage engines may use
        // padding, but we still require that they return the unpadded record data.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            if (results->valid) {
                // Only log once.
                results->errors.push_back("detected one or more invalid documents (see logs)");
            }
            nInvalid++;
            results->valid = false;
            log() << "document at location: " << record->id << " is corrupted";
        }

        prevRecordId = record->id;
    }

    if (results->valid) {
        recordStore->updateStatsAfterRepair(opCtx, nrecords, dataSizeTotal);
    }

    output->append("nInvalidDocuments", nInvalid);
    output->appendNumber("nrecords", nrecords);
}

void _validateRecordStore(OperationContext* opCtx,
                          RecordStore* recordStore,
                          ValidateCmdLevel level,
                          bool background,
                          RecordStoreValidateAdaptor* indexValidator,
                          ValidateResults* results,
                          BSONObjBuilder* output) {

    // Validate RecordStore and, if `level == kValidateFull`, use the RecordStore's validate
    // function.
    if (background) {
        indexValidator->traverseRecordStore(recordStore, level, results, output);
    } else {
        recordStore->validate(opCtx, level, results, output);
        _genericRecordStoreValidate(opCtx, recordStore, indexValidator, results, output);
    }
}

void _validateIndexes(OperationContext* opCtx,
                      IndexCatalog* indexCatalog,
                      BSONObjBuilder* keysPerIndex,
                      RecordStoreValidateAdaptor* indexValidator,
                      ValidateCmdLevel level,
                      ValidateResultsMap* indexNsResultsMap,
                      ValidateResults* results) {

    std::unique_ptr<IndexCatalog::IndexIterator> it = indexCatalog->getIndexIterator(opCtx, false);

    // Validate Indexes.
    while (it->more()) {
        opCtx->checkForInterrupt();
        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        log(LogComponent::kIndex) << "validating index " << descriptor->indexName()
                                  << " on collection " << descriptor->parentNS();
        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];
        bool checkCounts = false;
        int64_t numTraversedKeys;
        int64_t numValidatedKeys;

        if (level == kValidateFull) {
            iam->validate(opCtx, &numValidatedKeys, &curIndexResults);
            checkCounts = true;
        }

        if (curIndexResults.valid) {
            indexValidator->traverseIndex(iam, descriptor, &curIndexResults, &numTraversedKeys);

            if (checkCounts && (numValidatedKeys != numTraversedKeys)) {
                curIndexResults.valid = false;
                string msg = str::stream()
                    << "number of traversed index entries (" << numTraversedKeys
                    << ") does not match the number of expected index entries (" << numValidatedKeys
                    << ")";
                results->errors.push_back(msg);
                results->valid = false;
            }

            if (curIndexResults.valid) {
                keysPerIndex->appendNumber(descriptor->indexName(),
                                           static_cast<long long>(numTraversedKeys));
            } else {
                results->valid = false;
            }
        } else {
            results->valid = false;
        }
    }
}

/**
 * Executes the second phase of validation for improved error reporting. This is only done if
 * any index inconsistencies are found during the first phase of validation.
 */
void _gatherIndexEntryErrors(OperationContext* opCtx,
                             RecordStore* recordStore,
                             IndexCatalog* indexCatalog,
                             IndexConsistency* indexConsistency,
                             RecordStoreValidateAdaptor* indexValidator,
                             ValidateResultsMap* indexNsResultsMap,
                             ValidateResults* result) {
    indexConsistency->setSecondPhase();

    log(LogComponent::kIndex) << "Starting to traverse through all the document key sets.";

    // During the second phase of validation, iterate through each documents key set and only record
    // the keys that were inconsistent during the first phase of validation.
    std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx, true);
    while (auto record = cursor->next()) {
        opCtx->checkForInterrupt();

        // We can ignore the status of validate as it was already checked during the first phase.
        size_t validatedSize;
        indexValidator->validate(record->id, record->data, &validatedSize).ignore();
    }

    log(LogComponent::kIndex) << "Finished traversing through all the document key sets.";
    log(LogComponent::kIndex) << "Starting to traverse through all the indexes.";

    // Iterate through all the indexes in the collection and only record the index entry keys that
    // had inconsistencies during the first phase.
    std::unique_ptr<IndexCatalog::IndexIterator> it = indexCatalog->getIndexIterator(opCtx, false);
    while (it->more()) {
        opCtx->checkForInterrupt();

        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        log(LogComponent::kIndex) << "Traversing through the index entries for index "
                                  << descriptor->indexName() << ".";
        indexValidator->traverseIndex(
            iam, descriptor, /*ValidateResults=*/nullptr, /*numTraversedKeys=*/nullptr);
    }

    log(LogComponent::kIndex) << "Finished traversing through all the indexes.";

    indexConsistency->addIndexEntryErrors(indexNsResultsMap, result);
}

void _validateIndexKeyCount(OperationContext* opCtx,
                            IndexCatalog* indexCatalog,
                            RecordStore* recordStore,
                            RecordStoreValidateAdaptor* indexValidator,
                            ValidateResultsMap* indexNsResultsMap) {

    std::unique_ptr<IndexCatalog::IndexIterator> indexIterator =
        indexCatalog->getIndexIterator(opCtx, false);
    while (indexIterator->more()) {
        const IndexDescriptor* descriptor = indexIterator->next()->descriptor();
        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];

        if (curIndexResults.valid) {
            indexValidator->validateIndexKeyCount(
                descriptor, recordStore->numRecords(opCtx), curIndexResults);
        }
    }
}

void _reportValidationResults(OperationContext* opCtx,
                              IndexCatalog* indexCatalog,
                              ValidateResultsMap* indexNsResultsMap,
                              BSONObjBuilder* keysPerIndex,
                              ValidateCmdLevel level,
                              ValidateResults* results,
                              BSONObjBuilder* output) {

    std::unique_ptr<BSONObjBuilder> indexDetails;
    if (level == kValidateFull) {
        indexDetails = stdx::make_unique<BSONObjBuilder>();
    }

    // Report index validation results.
    for (const auto& it : *indexNsResultsMap) {
        const std::string indexNs = it.first;
        const ValidateResults& vr = it.second;

        if (!vr.valid) {
            results->valid = false;
        }

        if (indexDetails.get()) {
            BSONObjBuilder bob(indexDetails->subobjStart(indexNs));
            bob.appendBool("valid", vr.valid);

            if (!vr.warnings.empty()) {
                bob.append("warnings", vr.warnings);
            }

            if (!vr.errors.empty()) {
                bob.append("errors", vr.errors);
            }
        }

        results->warnings.insert(results->warnings.end(), vr.warnings.begin(), vr.warnings.end());
        results->errors.insert(results->errors.end(), vr.errors.begin(), vr.errors.end());
    }

    output->append("nIndexes", indexCatalog->numIndexesReady(opCtx));
    output->append("keysPerIndex", keysPerIndex->done());
    if (indexDetails.get()) {
        output->append("indexDetails", indexDetails->done());
    }
}
template <typename T>
void addErrorIfUnequal(T stored, T cached, StringData name, ValidateResults* results) {
    if (stored != cached) {
        results->valid = false;
        results->errors.push_back(str::stream() << "stored value for " << name
                                                << " does not match cached value: " << stored
                                                << " != " << cached);
    }
}

std::string multikeyPathsToString(MultikeyPaths paths) {
    str::stream builder;
    builder << "[";
    auto pathIt = paths.begin();
    while (true) {
        builder << "{";

        auto pathSet = *pathIt;
        auto setIt = pathSet.begin();
        while (true) {
            builder << *setIt++;
            if (setIt == pathSet.end()) {
                break;
            } else {
                builder << ",";
            }
        }
        builder << "}";

        if (++pathIt == paths.end()) {
            break;
        } else {
            builder << ",";
        }
    }
    builder << "]";
    return builder;
}

void _validateCatalogEntry(OperationContext* opCtx,
                           CollectionImpl* coll,
                           BSONObj validatorDoc,
                           ValidateResults* results) {
    CollectionOptions options = DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, coll->ns());
    addErrorIfUnequal(options.uuid, coll->uuid(), "UUID", results);
    const CollatorInterface* collation = coll->getDefaultCollator();
    addErrorIfUnequal(options.collation.isEmpty(), !collation, "simple collation", results);
    if (!options.collation.isEmpty() && collation)
        addErrorIfUnequal(options.collation.toString(),
                          collation->getSpec().toBSON().toString(),
                          "collation",
                          results);
    addErrorIfUnequal(options.capped, coll->isCapped(), "is capped", results);

    addErrorIfUnequal(options.validator.toString(), validatorDoc.toString(), "validator", results);
    if (!options.validator.isEmpty() && !validatorDoc.isEmpty()) {
        addErrorIfUnequal(options.validationAction.length() ? options.validationAction : "error",
                          coll->getValidationAction().toString(),
                          "validation action",
                          results);
        addErrorIfUnequal(options.validationLevel.length() ? options.validationLevel : "strict",
                          coll->getValidationLevel().toString(),
                          "validation level",
                          results);
    }

    addErrorIfUnequal(options.isView(), false, "is a view", results);
    auto status = options.validateForStorage();
    if (!status.isOK()) {
        results->valid = false;
        results->errors.push_back(str::stream() << "collection options are not valid for storage: "
                                                << options.toBSON());
    }

    std::vector<std::string> indexes;
    DurableCatalog::get(opCtx)->getReadyIndexes(opCtx, coll->ns(), &indexes);
    for (auto& index : indexes) {
        MultikeyPaths multikeyPaths;
        const bool isMultikey =
            DurableCatalog::get(opCtx)->isIndexMultikey(opCtx, coll->ns(), index, &multikeyPaths);
        const bool hasMultiKeyPaths = std::any_of(multikeyPaths.begin(),
                                                  multikeyPaths.end(),
                                                  [](auto& pathSet) { return pathSet.size() > 0; });
        // It is illegal for multikey paths to exist without the multikey flag set on the index, but
        // it may be possible for multikey to be set on the index while having no multikey paths. If
        // any of the paths are multikey, then the entire index should also be marked multikey.
        if (hasMultiKeyPaths && !isMultikey) {
            results->valid = false;
            results->errors.push_back(fmt::format(
                "The 'multikey' field for index {} was false with non-empty 'multikeyPaths': {}",
                index,
                multikeyPathsToString(multikeyPaths)));
        }
    }
}

}  // namespace

Status CollectionImpl::validate(OperationContext* opCtx,
                                ValidateCmdLevel level,
                                bool background,
                                ValidateResults* results,
                                BSONObjBuilder* output) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns(), MODE_IS));

    const auto nss = NamespaceString(ns());
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    // Check whether we are allowed to read from this node after acquiring our locks. If we are
    // in a state where we cannot read, we should not run validate.
    uassertStatusOK(replCoord->checkCanServeReadsFor(
        opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

    try {
        ValidateResultsMap indexNsResultsMap;
        BSONObjBuilder keysPerIndex;  // not using subObjStart to be exception safe
        IndexConsistency indexConsistency(opCtx, this, ns(), _recordStore.get(), background);
        RecordStoreValidateAdaptor indexValidator = RecordStoreValidateAdaptor(
            opCtx, &indexConsistency, level, _indexCatalog.get(), &indexNsResultsMap);

        // Validate the record store
        std::string uuidString = str::stream()
            << " (UUID: " << (uuid() ? uuid()->toString() : "none") << ")";
        log(LogComponent::kIndex) << "validating collection " << ns() << uuidString;
        _validateRecordStore(
            opCtx, _recordStore.get(), level, background, &indexValidator, results, output);

        // Validate in-memory catalog information with the persisted info.
        _validateCatalogEntry(opCtx, this, _validatorDoc, results);

        // Validate indexes and check for mismatches.
        if (results->valid) {
            _validateIndexes(opCtx,
                             _indexCatalog.get(),
                             &keysPerIndex,
                             &indexValidator,
                             level,
                             &indexNsResultsMap,
                             results);

            if (indexConsistency.haveEntryMismatch()) {
                log(LogComponent::kIndex)
                    << "Index inconsistencies were detected on collection " << ns()
                    << ". Starting the second phase of index validation to gather concise errors.";
                _gatherIndexEntryErrors(opCtx,
                                        _recordStore.get(),
                                        _indexCatalog.get(),
                                        &indexConsistency,
                                        &indexValidator,
                                        &indexNsResultsMap,
                                        results);
            }
        }

        // Validate index key count.
        if (results->valid) {
            _validateIndexKeyCount(opCtx,
                                   _indexCatalog.get(),
                                   _recordStore.get(),
                                   &indexValidator,
                                   &indexNsResultsMap);
        }

        // Report the validation results for the user to see
        _reportValidationResults(
            opCtx, _indexCatalog.get(), &indexNsResultsMap, &keysPerIndex, level, results, output);

        if (!results->valid) {
            log(LogComponent::kIndex)
                << "validating collection " << ns() << " failed" << uuidString;
        } else {
            log(LogComponent::kIndex) << "validated collection " << ns() << uuidString;
        }
    } catch (DBException& e) {
        if (ErrorCodes::isInterruption(e.code())) {
            return e.toStatus();
        }
        string err = str::stream() << "exception during index validation: " << e.toString();
        results->errors.push_back(err);
        results->valid = false;
    }

    return Status::OK();
}

Status CollectionImpl::touch(OperationContext* opCtx,
                             bool touchData,
                             bool touchIndexes,
                             BSONObjBuilder* output) const {
    if (touchData) {
        BSONObjBuilder b;
        Status status = _recordStore.get()->touch(opCtx, &b);
        if (!status.isOK())
            return status;
        output->append("data", b.obj());
    }

    if (touchIndexes) {
        Timer t;
        std::unique_ptr<IndexCatalog::IndexIterator> ii =
            _indexCatalog->getIndexIterator(opCtx, false);
        while (ii->more()) {
            const IndexCatalogEntry* entry = ii->next();
            const IndexAccessMethod* iam = entry->accessMethod();
            Status status = iam->touch(opCtx);
            if (!status.isOK())
                return status;
        }

        output->append(
            "indexes",
            BSON("num" << _indexCatalog->numIndexesTotal(opCtx) << "millis" << t.millis()));
    }

    return Status::OK();
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> CollectionImpl::makePlanExecutor(
    OperationContext* opCtx, PlanExecutor::YieldPolicy yieldPolicy, ScanDirection scanDirection) {
    auto isForward = scanDirection == ScanDirection::kForward;
    auto direction = isForward ? InternalPlanner::FORWARD : InternalPlanner::BACKWARD;
    return InternalPlanner::collectionScan(opCtx, _ns.ns(), this, yieldPolicy, direction);
}

void CollectionImpl::setNs(NamespaceString nss) {
    _ns = std::move(nss);
    _indexCatalog->setNs(_ns);
    _infoCache->setNs(_ns);
    _recordStore.get()->setNs(_ns);
}

void CollectionImpl::indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) {
    DurableCatalog::get(opCtx)->indexBuildSuccess(opCtx, ns(), index->descriptor()->indexName());
    _indexCatalog->indexBuildSuccess(opCtx, index);
}

void CollectionImpl::establishOplogCollectionForLogging(OperationContext* opCtx) {
    repl::establishOplogCollectionForLogging(opCtx, this);
}

}  // namespace mongo
