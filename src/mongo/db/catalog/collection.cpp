// collection.cpp

/**
*    Copyright (C) 2013-2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"

#include "mongo/base/counter.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

#include "mongo/db/auth/user_document_parser.h"  // XXX-ANDY
#include "mongo/util/log.h"

namespace mongo {

namespace {
const auto bannedExpressionsInValidators = std::set<StringData>{
    "$geoNear", "$near", "$nearSphere", "$text", "$where",
};

Status checkValidatorForBannedExpressions(const BSONObj& validator) {
    for (auto field : validator) {
        const auto name = field.fieldNameStringData();
        if (name[0] == '$' && bannedExpressionsInValidators.count(name)) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << name << " is not allowed in collection validators"};
        }

        if (field.type() == Object || field.type() == Array) {
            auto status = checkValidatorForBannedExpressions(field.Obj());
            if (!status.isOK())
                return status;
        }
    }

    return Status::OK();
}
}

using std::unique_ptr;
using std::endl;
using std::string;
using std::vector;

using logger::LogComponent;

static const int IndexKeyMaxSize = 1024;  // this goes away with SERVER-3372

std::string CompactOptions::toString() const {
    std::stringstream ss;
    ss << "paddingMode: ";
    switch (paddingMode) {
        case NONE:
            ss << "NONE";
            break;
        case PRESERVE:
            ss << "PRESERVE";
            break;
        case MANUAL:
            ss << "MANUAL (" << paddingBytes << " + ( doc * " << paddingFactor << ") )";
    }

    ss << " validateDocuments: " << validateDocuments;

    return ss.str();
}

//
// CappedInsertNotifier
//

CappedInsertNotifier::CappedInsertNotifier() : _version(0), _dead(false) {}

void CappedInsertNotifier::notifyAll() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ++_version;
    _notifier.notify_all();
}

void CappedInsertNotifier::_wait(stdx::unique_lock<stdx::mutex>& lk,
                                 uint64_t prevVersion,
                                 Microseconds timeout) const {
    while (!_dead && prevVersion == _version) {
        if (timeout == Microseconds::max()) {
            _notifier.wait(lk);
        } else if (stdx::cv_status::timeout == _notifier.wait_for(lk, timeout)) {
            return;
        }
    }
}

void CappedInsertNotifier::wait(uint64_t prevVersion, Microseconds timeout) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _wait(lk, prevVersion, timeout);
}

void CappedInsertNotifier::wait(Microseconds timeout) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _wait(lk, _version, timeout);
}

void CappedInsertNotifier::wait() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _wait(lk, _version, Microseconds::max());
}

void CappedInsertNotifier::kill() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dead = true;
    _notifier.notify_all();
}

bool CappedInsertNotifier::isDead() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _dead;
}

// ----

Collection::Collection(OperationContext* txn,
                       StringData fullNS,
                       CollectionCatalogEntry* details,
                       RecordStore* recordStore,
                       DatabaseCatalogEntry* dbce)
    : _ns(fullNS),
      _details(details),
      _recordStore(recordStore),
      _dbce(dbce),
      _needCappedLock(supportsDocLocking() && _recordStore->isCapped() && _ns.db() != "local"),
      _infoCache(this),
      _indexCatalog(this),
      _validatorDoc(_details->getCollectionOptions(txn).validator.getOwned()),
      _validator(uassertStatusOK(parseValidator(_validatorDoc))),
      _validationAction(uassertStatusOK(
          _parseValidationAction(_details->getCollectionOptions(txn).validationAction))),
      _validationLevel(uassertStatusOK(
          _parseValidationLevel(_details->getCollectionOptions(txn).validationLevel))),
      _cursorManager(fullNS),
      _cappedNotifier(_recordStore->isCapped() ? new CappedInsertNotifier() : nullptr),
      _mustTakeCappedLockOnInsert(isCapped() && !_ns.isSystemDotProfile() && !_ns.isOplog()) {
    _magic = 1357924;
    _indexCatalog.init(txn);
    if (isCapped())
        _recordStore->setCappedCallback(this);

    _infoCache.init(txn);
}

Collection::~Collection() {
    verify(ok());
    _magic = 0;
    if (_cappedNotifier) {
        _cappedNotifier->kill();
    }
}

bool Collection::requiresIdIndex() const {
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

std::unique_ptr<SeekableRecordCursor> Collection::getCursor(OperationContext* txn,
                                                            bool forward) const {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IS));
    invariant(ok());

    return _recordStore->getCursor(txn, forward);
}

vector<std::unique_ptr<RecordCursor>> Collection::getManyCursors(OperationContext* txn) const {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IS));

    return _recordStore->getManyCursors(txn);
}

Snapshotted<BSONObj> Collection::docFor(OperationContext* txn, const RecordId& loc) const {
    return Snapshotted<BSONObj>(txn->recoveryUnit()->getSnapshotId(),
                                _recordStore->dataFor(txn, loc).releaseToBson());
}

bool Collection::findDoc(OperationContext* txn,
                         const RecordId& loc,
                         Snapshotted<BSONObj>* out) const {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IS));

    RecordData rd;
    if (!_recordStore->findRecord(txn, loc, &rd))
        return false;
    *out = Snapshotted<BSONObj>(txn->recoveryUnit()->getSnapshotId(), rd.releaseToBson());
    return true;
}

Status Collection::checkValidation(OperationContext* txn, const BSONObj& document) const {
    if (!_validator)
        return Status::OK();

    if (_validationLevel == OFF)
        return Status::OK();

    if (documentValidationDisabled(txn))
        return Status::OK();

    if (_validator->matchesBSON(document))
        return Status::OK();

    if (_validationAction == WARN) {
        warning() << "Document would fail validation"
                  << " collection: " << ns() << " doc: " << document;
        return Status::OK();
    }

    return {ErrorCodes::DocumentValidationFailure, "Document failed validation"};
}

StatusWithMatchExpression Collection::parseValidator(const BSONObj& validator) const {
    if (validator.isEmpty())
        return {nullptr};

    if (ns().isSystem()) {
        return {ErrorCodes::InvalidOptions,
                "Document validators not allowed on system collections."};
    }

    if (ns().isOnInternalDb()) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Document validators are not allowed on collections in"
                              << " the " << ns().db() << " database"};
    }

    {
        auto status = checkValidatorForBannedExpressions(validator);
        if (!status.isOK())
            return status;
    }

    auto statusWithMatcher =
        MatchExpressionParser::parse(validator, ExtensionsCallbackDisallowExtensions());
    if (!statusWithMatcher.isOK())
        return statusWithMatcher.getStatus();

    return statusWithMatcher;
}

Status Collection::insertDocument(OperationContext* txn, const DocWriter* doc, bool enforceQuota) {
    invariant(!_validator || documentValidationDisabled(txn));
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));
    invariant(!_indexCatalog.haveAnyIndexes());  // eventually can implement, just not done

    if (_mustTakeCappedLockOnInsert)
        synchronizeOnCappedInFlightResource(txn->lockState(), _ns);

    StatusWith<RecordId> loc = _recordStore->insertRecord(txn, doc, _enforceQuota(enforceQuota));
    if (!loc.isOK())
        return loc.getStatus();

    // we cannot call into the OpObserver here because the document being written is not present
    // fortunately, this is currently only used for adding entries to the oplog.

    txn->recoveryUnit()->onCommit([this]() { notifyCappedWaitersIfNeeded(); });

    return loc.getStatus();
}


Status Collection::insertDocuments(OperationContext* txn,
                                   const vector<BSONObj>::const_iterator begin,
                                   const vector<BSONObj>::const_iterator end,
                                   OpDebug* opDebug,
                                   bool enforceQuota,
                                   bool fromMigrate) {
    // Should really be done in the collection object at creation and updated on index create.
    const bool hasIdIndex = _indexCatalog.findIdIndex(txn);

    for (auto it = begin; it != end; it++) {
        if (hasIdIndex && (*it)["_id"].eoo()) {
            return Status(ErrorCodes::InternalError,
                          str::stream() << "Collection::insertDocument got "
                                           "document without _id for ns:" << _ns.ns());
        }

        auto status = checkValidation(txn, *it);
        if (!status.isOK())
            return status;
    }

    const SnapshotId sid = txn->recoveryUnit()->getSnapshotId();

    if (_mustTakeCappedLockOnInsert)
        synchronizeOnCappedInFlightResource(txn->lockState(), _ns);

    Status status = _insertDocuments(txn, begin, end, enforceQuota, opDebug);
    if (!status.isOK())
        return status;
    invariant(sid == txn->recoveryUnit()->getSnapshotId());

    auto opObserver = getGlobalServiceContext()->getOpObserver();
    if (opObserver)
        opObserver->onInserts(txn, ns(), begin, end, fromMigrate);

    txn->recoveryUnit()->onCommit([this]() { notifyCappedWaitersIfNeeded(); });

    return Status::OK();
}

Status Collection::insertDocument(OperationContext* txn,
                                  const BSONObj& docToInsert,
                                  OpDebug* opDebug,
                                  bool enforceQuota,
                                  bool fromMigrate) {
    vector<BSONObj> docs;
    docs.push_back(docToInsert);
    return insertDocuments(txn, docs.begin(), docs.end(), opDebug, enforceQuota, fromMigrate);
}

Status Collection::insertDocument(OperationContext* txn,
                                  const BSONObj& doc,
                                  MultiIndexBlock* indexBlock,
                                  bool enforceQuota) {
    {
        auto status = checkValidation(txn, doc);
        if (!status.isOK())
            return status;
    }

    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));

    if (_mustTakeCappedLockOnInsert)
        synchronizeOnCappedInFlightResource(txn->lockState(), _ns);

    StatusWith<RecordId> loc =
        _recordStore->insertRecord(txn, doc.objdata(), doc.objsize(), _enforceQuota(enforceQuota));

    if (!loc.isOK())
        return loc.getStatus();

    Status status = indexBlock->insert(doc, loc.getValue());
    if (!status.isOK())
        return status;

    vector<BSONObj> docs;
    docs.push_back(doc);

    auto opObserver = getGlobalServiceContext()->getOpObserver();
    if (opObserver)
        opObserver->onInserts(txn, ns(), docs.begin(), docs.end());

    txn->recoveryUnit()->onCommit([this]() { notifyCappedWaitersIfNeeded(); });

    return loc.getStatus();
}

Status Collection::_insertDocuments(OperationContext* txn,
                                    const vector<BSONObj>::const_iterator begin,
                                    const vector<BSONObj>::const_iterator end,
                                    bool enforceQuota,
                                    OpDebug* opDebug) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));

    if (isCapped() && _indexCatalog.haveAnyIndexes() && std::distance(begin, end) > 1) {
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
        Lock::ResourceLock{txn->lockState(), ResourceId(RESOURCE_METADATA, _ns.ns()), MODE_X};
    }

    std::vector<Record> records;
    for (auto it = begin; it != end; it++) {
        Record record = {RecordId(), RecordData(it->objdata(), it->objsize())};
        records.push_back(record);
    }
    Status status = _recordStore->insertRecords(txn, &records, _enforceQuota(enforceQuota));
    if (!status.isOK())
        return status;

    std::vector<BsonRecord> bsonRecords;
    int recordIndex = 0;
    for (auto it = begin; it != end; it++) {
        RecordId loc = records[recordIndex++].id;
        invariant(RecordId::min() < loc);
        invariant(loc < RecordId::max());

        BsonRecord bsonRecord = {loc, &(*it)};
        bsonRecords.push_back(bsonRecord);
    }

    int64_t keysInserted;
    status = _indexCatalog.indexRecords(txn, bsonRecords, &keysInserted);
    if (opDebug) {
        opDebug->keysInserted += keysInserted;
    }

    return status;
}

void Collection::notifyCappedWaitersIfNeeded() {
    // If there is a notifier object and another thread is waiting on it, then we notify
    // waiters of this document insert. Waiters keep a shared_ptr to '_cappedNotifier', so
    // there are waiters if this Collection's shared_ptr is not unique (use_count > 1).
    if (_cappedNotifier && !_cappedNotifier.unique())
        _cappedNotifier->notifyAll();
}

Status Collection::aboutToDeleteCapped(OperationContext* txn,
                                       const RecordId& loc,
                                       RecordData data) {
    /* check if any cursors point to us.  if so, advance them. */
    _cursorManager.invalidateDocument(txn, loc, INVALIDATION_DELETION);

    BSONObj doc = data.releaseToBson();
    int64_t* const nullKeysDeleted = nullptr;
    _indexCatalog.unindexRecord(txn, doc, loc, false, nullKeysDeleted);

    // We are not capturing and reporting to OpDebug the 'keysDeleted' by unindexRecord(). It is
    // questionable whether reporting will add diagnostic value to users and may instead be
    // confusing as it depends on our internal capped collection document removal strategy.
    // We can consider adding either keysDeleted or a new metric reporting document removal if
    // justified by user demand.

    return Status::OK();
}

void Collection::deleteDocument(OperationContext* txn,
                                const RecordId& loc,
                                OpDebug* opDebug,
                                bool fromMigrate,
                                bool noWarn) {
    if (isCapped()) {
        log() << "failing remove on a capped ns " << _ns << endl;
        uasserted(10089, "cannot remove from a capped collection");
        return;
    }

    Snapshotted<BSONObj> doc = docFor(txn, loc);

    OpObserver::DeleteState deleteState;
    auto opObserver = getGlobalServiceContext()->getOpObserver();
    if (opObserver)
        deleteState = opObserver->aboutToDelete(txn, ns(), doc.value());

    /* check if any cursors point to us.  if so, advance them. */
    _cursorManager.invalidateDocument(txn, loc, INVALIDATION_DELETION);

    int64_t keysDeleted;
    _indexCatalog.unindexRecord(txn, doc.value(), loc, noWarn, &keysDeleted);
    if (opDebug) {
        opDebug->keysDeleted += keysDeleted;
    }

    _recordStore->deleteRecord(txn, loc);

    if (opObserver)
        opObserver->onDelete(txn, ns(), std::move(deleteState), fromMigrate);
}

Counter64 moveCounter;
ServerStatusMetricField<Counter64> moveCounterDisplay("record.moves", &moveCounter);

StatusWith<RecordId> Collection::updateDocument(OperationContext* txn,
                                                const RecordId& oldLocation,
                                                const Snapshotted<BSONObj>& oldDoc,
                                                const BSONObj& newDoc,
                                                bool enforceQuota,
                                                bool indexesAffected,
                                                OpDebug* opDebug,
                                                OplogUpdateEntryArgs* args) {
    {
        auto status = checkValidation(txn, newDoc);
        if (!status.isOK()) {
            if (_validationLevel == STRICT_V) {
                return status;
            }
            // moderate means we have to check the old doc
            auto oldDocStatus = checkValidation(txn, oldDoc.value());
            if (oldDocStatus.isOK()) {
                // transitioning from good -> bad is not ok
                return status;
            }
            // bad -> bad is ok in moderate mode
        }
    }

    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));
    invariant(oldDoc.snapshotId() == txn->recoveryUnit()->getSnapshotId());
    invariant(newDoc.isOwned());

    if (_needCappedLock) {
        // X-lock the metadata resource for this capped collection until the end of the WUOW. This
        // prevents the primary from executing with more concurrency than secondaries.
        // See SERVER-21646.
        Lock::ResourceLock{txn->lockState(), ResourceId(RESOURCE_METADATA, _ns.ns()), MODE_X};
    }

    SnapshotId sid = txn->recoveryUnit()->getSnapshotId();

    BSONElement oldId = oldDoc.value()["_id"];
    if (!oldId.eoo() && (oldId != newDoc["_id"]))
        return StatusWith<RecordId>(
            ErrorCodes::InternalError, "in Collection::updateDocument _id mismatch", 13596);

    // The MMAPv1 storage engine implements capped collections in a way that does not allow records
    // to grow beyond their original size. If MMAPv1 part of a replicaset with storage engines that
    // do not have this limitation, replication could result in errors, so it is necessary to set a
    // uniform rule here. Similarly, it is not sufficient to disallow growing records, because this
    // happens when secondaries roll back an update shrunk a record. Exactly replicating legacy
    // MMAPv1 behavior would require padding shrunk documents on all storage engines. Instead forbid
    // all size changes.
    const auto oldSize = oldDoc.value().objsize();
    if (_recordStore->isCapped() && oldSize != newDoc.objsize())
        return {ErrorCodes::CannotGrowDocumentInCappedNamespace,
                str::stream() << "Cannot change the size of a document in a capped collection: "
                              << oldSize << " != " << newDoc.objsize()};

    // At the end of this step, we will have a map of UpdateTickets, one per index, which
    // represent the index updates needed to be done, based on the changes between oldDoc and
    // newDoc.
    OwnedPointerMap<IndexDescriptor*, UpdateTicket> updateTickets;
    if (indexesAffected) {
        IndexCatalog::IndexIterator ii = _indexCatalog.getIndexIterator(txn, true);
        while (ii.more()) {
            IndexDescriptor* descriptor = ii.next();
            IndexCatalogEntry* entry = ii.catalogEntry(descriptor);
            IndexAccessMethod* iam = ii.accessMethod(descriptor);

            InsertDeleteOptions options;
            options.logIfError = false;
            options.dupsAllowed =
                !(KeyPattern::isIdKeyPattern(descriptor->keyPattern()) || descriptor->unique()) ||
                repl::getGlobalReplicationCoordinator()->shouldIgnoreUniqueIndex(descriptor);
            UpdateTicket* updateTicket = new UpdateTicket();
            updateTickets.mutableMap()[descriptor] = updateTicket;
            Status ret = iam->validateUpdate(txn,
                                             oldDoc.value(),
                                             newDoc,
                                             oldLocation,
                                             options,
                                             updateTicket,
                                             entry->getFilterExpression());
            if (!ret.isOK()) {
                return StatusWith<RecordId>(ret);
            }
        }
    }

    Status updateStatus = _recordStore->updateRecord(
        txn, oldLocation, newDoc.objdata(), newDoc.objsize(), _enforceQuota(enforceQuota), this);

    if (updateStatus == ErrorCodes::NeedsDocumentMove) {
        return _updateDocumentWithMove(
            txn, oldLocation, oldDoc, newDoc, enforceQuota, opDebug, args, sid);
    } else if (!updateStatus.isOK()) {
        return updateStatus;
    }

    // Object did not move.  We update each index with each respective UpdateTicket.
    if (opDebug)
        opDebug->keyUpdates = 0;

    if (indexesAffected) {
        IndexCatalog::IndexIterator ii = _indexCatalog.getIndexIterator(txn, true);
        while (ii.more()) {
            IndexDescriptor* descriptor = ii.next();
            IndexAccessMethod* iam = ii.accessMethod(descriptor);

            int64_t keysInserted;
            int64_t keysDeleted;
            Status ret = iam->update(
                txn, *updateTickets.mutableMap()[descriptor], &keysInserted, &keysDeleted);
            if (!ret.isOK())
                return StatusWith<RecordId>(ret);
            if (opDebug) {
                opDebug->keyUpdates += keysInserted;
                opDebug->keysInserted += keysInserted;
                opDebug->keysDeleted += keysDeleted;
            }
        }
    }

    invariant(sid == txn->recoveryUnit()->getSnapshotId());
    args->updatedDoc = newDoc;

    auto opObserver = getGlobalServiceContext()->getOpObserver();
    if (opObserver)
        opObserver->onUpdate(txn, *args);

    return {oldLocation};
}

StatusWith<RecordId> Collection::_updateDocumentWithMove(OperationContext* txn,
                                                         const RecordId& oldLocation,
                                                         const Snapshotted<BSONObj>& oldDoc,
                                                         const BSONObj& newDoc,
                                                         bool enforceQuota,
                                                         OpDebug* opDebug,
                                                         OplogUpdateEntryArgs* args,
                                                         const SnapshotId& sid) {
    // Insert new record.
    StatusWith<RecordId> newLocation = _recordStore->insertRecord(
        txn, newDoc.objdata(), newDoc.objsize(), _enforceQuota(enforceQuota));
    if (!newLocation.isOK()) {
        return newLocation;
    }

    invariant(newLocation.getValue() != oldLocation);

    _cursorManager.invalidateDocument(txn, oldLocation, INVALIDATION_DELETION);

    // Remove indexes for old record.
    int64_t keysDeleted;
    _indexCatalog.unindexRecord(txn, oldDoc.value(), oldLocation, true, &keysDeleted);

    // Remove old record.
    _recordStore->deleteRecord(txn, oldLocation);

    std::vector<BsonRecord> bsonRecords;
    BsonRecord bsonRecord = {newLocation.getValue(), &newDoc};
    bsonRecords.push_back(bsonRecord);

    // Add indexes for new record.
    int64_t keysInserted;
    Status status = _indexCatalog.indexRecords(txn, bsonRecords, &keysInserted);
    if (!status.isOK()) {
        return StatusWith<RecordId>(status);
    }

    invariant(sid == txn->recoveryUnit()->getSnapshotId());
    args->updatedDoc = newDoc;
    txn->getServiceContext()->getOpObserver()->onUpdate(txn, *args);

    moveCounter.increment();
    if (opDebug) {
        opDebug->nmoved++;
        opDebug->keysInserted += keysInserted;
        opDebug->keysDeleted += keysDeleted;
    }

    return newLocation;
}

Status Collection::recordStoreGoingToUpdateInPlace(OperationContext* txn, const RecordId& loc) {
    // Broadcast the mutation so that query results stay correct.
    _cursorManager.invalidateDocument(txn, loc, INVALIDATION_MUTATION);
    return Status::OK();
}


bool Collection::updateWithDamagesSupported() const {
    if (_validator)
        return false;

    return _recordStore->updateWithDamagesSupported();
}

StatusWith<RecordData> Collection::updateDocumentWithDamages(
    OperationContext* txn,
    const RecordId& loc,
    const Snapshotted<RecordData>& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages,
    OplogUpdateEntryArgs* args) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));
    invariant(oldRec.snapshotId() == txn->recoveryUnit()->getSnapshotId());
    invariant(updateWithDamagesSupported());

    // Broadcast the mutation so that query results stay correct.
    _cursorManager.invalidateDocument(txn, loc, INVALIDATION_MUTATION);

    auto newRecStatus =
        _recordStore->updateWithDamages(txn, loc, oldRec.value(), damageSource, damages);

    if (newRecStatus.isOK()) {
        args->updatedDoc = newRecStatus.getValue().toBson();

        auto opObserver = getGlobalServiceContext()->getOpObserver();
        if (opObserver)
            opObserver->onUpdate(txn, *args);
    }
    return newRecStatus;
}

bool Collection::_enforceQuota(bool userEnforeQuota) const {
    if (!userEnforeQuota)
        return false;

    if (!mmapv1GlobalOptions.quota)
        return false;

    if (_ns.db() == "local")
        return false;

    if (_ns.isSpecial())
        return false;

    return true;
}

bool Collection::isCapped() const {
    return _cappedNotifier.get();
}

std::shared_ptr<CappedInsertNotifier> Collection::getCappedInsertNotifier() const {
    invariant(isCapped());
    return _cappedNotifier;
}

uint64_t Collection::numRecords(OperationContext* txn) const {
    return _recordStore->numRecords(txn);
}

uint64_t Collection::dataSize(OperationContext* txn) const {
    return _recordStore->dataSize(txn);
}

uint64_t Collection::getIndexSize(OperationContext* opCtx, BSONObjBuilder* details, int scale) {
    IndexCatalog* idxCatalog = getIndexCatalog();

    IndexCatalog::IndexIterator ii = idxCatalog->getIndexIterator(opCtx, true);

    uint64_t totalSize = 0;

    while (ii.more()) {
        IndexDescriptor* d = ii.next();
        IndexAccessMethod* iam = idxCatalog->getIndex(d);

        long long ds = iam->getSpaceUsedBytes(opCtx);

        totalSize += ds;
        if (details) {
            details->appendNumber(d->indexName(), ds / scale);
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
Status Collection::truncate(OperationContext* txn) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_X));
    BackgroundOperation::assertNoBgOpInProgForNs(ns());
    invariant(_indexCatalog.numIndexesInProgress(txn) == 0);

    // 1) store index specs
    vector<BSONObj> indexSpecs;
    {
        IndexCatalog::IndexIterator ii = _indexCatalog.getIndexIterator(txn, false);
        while (ii.more()) {
            const IndexDescriptor* idx = ii.next();
            indexSpecs.push_back(idx->infoObj().getOwned());
        }
    }

    // 2) drop indexes
    Status status = _indexCatalog.dropAllIndexes(txn, true);
    if (!status.isOK())
        return status;
    _cursorManager.invalidateAll(false, "collection truncated");

    // 3) truncate record store
    status = _recordStore->truncate(txn);
    if (!status.isOK())
        return status;

    // 4) re-create indexes
    for (size_t i = 0; i < indexSpecs.size(); i++) {
        status = _indexCatalog.createIndexOnEmptyCollection(txn, indexSpecs[i]);
        if (!status.isOK())
            return status;
    }

    return Status::OK();
}

void Collection::temp_cappedTruncateAfter(OperationContext* txn, RecordId end, bool inclusive) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));
    invariant(isCapped());
    BackgroundOperation::assertNoBgOpInProgForNs(ns());
    invariant(_indexCatalog.numIndexesInProgress(txn) == 0);

    _cursorManager.invalidateAll(false, "capped collection truncated");
    _recordStore->temp_cappedTruncateAfter(txn, end, inclusive);
}

Status Collection::setValidator(OperationContext* txn, BSONObj validatorDoc) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_X));

    // Make owned early so that the parsed match expression refers to the owned object.
    if (!validatorDoc.isOwned())
        validatorDoc = validatorDoc.getOwned();

    auto statusWithMatcher = parseValidator(validatorDoc);
    if (!statusWithMatcher.isOK())
        return statusWithMatcher.getStatus();

    _details->updateValidator(txn, validatorDoc, getValidationLevel(), getValidationAction());

    _validator = std::move(statusWithMatcher.getValue());
    _validatorDoc = std::move(validatorDoc);
    return Status::OK();
}

StatusWith<Collection::ValidationLevel> Collection::_parseValidationLevel(StringData newLevel) {
    if (newLevel == "") {
        // default
        return STRICT_V;
    } else if (newLevel == "off") {
        return OFF;
    } else if (newLevel == "moderate") {
        return MODERATE;
    } else if (newLevel == "strict") {
        return STRICT_V;
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "invalid validation level: " << newLevel);
    }
}

StatusWith<Collection::ValidationAction> Collection::_parseValidationAction(StringData newAction) {
    if (newAction == "") {
        // default
        return ERROR_V;
    } else if (newAction == "warn") {
        return WARN;
    } else if (newAction == "error") {
        return ERROR_V;
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "invalid validation action: " << newAction);
    }
}

StringData Collection::getValidationLevel() const {
    switch (_validationLevel) {
        case STRICT_V:
            return "strict";
        case OFF:
            return "off";
        case MODERATE:
            return "moderate";
    }
    MONGO_UNREACHABLE;
}

StringData Collection::getValidationAction() const {
    switch (_validationAction) {
        case ERROR_V:
            return "error";
        case WARN:
            return "warn";
    }
    MONGO_UNREACHABLE;
}

Status Collection::setValidationLevel(OperationContext* txn, StringData newLevel) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_X));

    StatusWith<ValidationLevel> status = _parseValidationLevel(newLevel);
    if (!status.isOK()) {
        return status.getStatus();
    }

    _validationLevel = status.getValue();

    _details->updateValidator(txn, _validatorDoc, getValidationLevel(), getValidationAction());

    return Status::OK();
}

Status Collection::setValidationAction(OperationContext* txn, StringData newAction) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_X));

    StatusWith<ValidationAction> status = _parseValidationAction(newAction);
    if (!status.isOK()) {
        return status.getStatus();
    }

    _validationAction = status.getValue();

    _details->updateValidator(txn, _validatorDoc, getValidationLevel(), getValidationAction());

    return Status::OK();
}

namespace {
class RecordDataValidateAdaptor : public ValidateAdaptor {
public:
    virtual ~RecordDataValidateAdaptor() {}

    virtual Status validate(const RecordData& record, size_t* dataSize) {
        BSONObj obj = record.toBson();
        const Status status = validateBSON(obj.objdata(), obj.objsize());
        if (status.isOK())
            *dataSize = obj.objsize();
        return status;
    }
};

using IndexKeyCountMap = std::unordered_map<uint32_t, uint64_t>;
using ValidateResultsMap = std::map<std::string, ValidateResults>;

class IndexValidator {
public:
    IndexValidator(bool full) : _full(full), _ikc(new IndexKeyCountMap()) {}

    /**
     * Create one hash map of all entries for all indexes in a collection.
     * This ensures that we only need to do one collection scan when validating
     * a document against all the indexes that point to it.
     */
    void cacheIndexInfo(OperationContext* txn,
                        const IndexAccessMethod* iam,
                        const IndexDescriptor* descriptor,
                        long long numKeys) {
        _keyCounts[descriptor->indexNamespace()] = numKeys;

        if (_full) {
            const auto& key = descriptor->keyPattern();
            const bool forward = key.firstElement().number() >= 0;
            std::unique_ptr<SortedDataInterface::Cursor> cursor = iam->newCursor(txn, forward);
            cursor->setEndPosition(kMaxBSONKey, true);
            auto indexEntry = cursor->seek(kMinBSONKey, true);
            while (indexEntry) {
                uint32_t keyHash = hashIndexEntry(indexEntry->key, indexEntry->loc);
                (*_ikc)[keyHash]++;
                indexEntry = cursor->next();
            }
        }
    }

    void validateIndexes(OperationContext* txn,
                         RecordStore* rs,
                         IndexCatalog& indexCatalog,
                         ValidateResultsMap& indexDetailNsResultsMap,
                         ValidateResults* globalResults) {
        if (!indexCatalog.haveAnyIndexes()) {
            return;
        }
        auto documentCursor = rs->getCursor(txn, true);
        // Only used to determine if any index validation failed in this function,
        // doesn't include any indexes that have already been marked as invalid.
        bool allIndexesValid = true;
        uint64_t documentCount = 0;
        int interruptInterval = 4096;
        if (_full) {
            while (const auto& document = documentCursor->next()) {
                if (!(documentCount % interruptInterval))
                    txn->checkForInterrupt();
                documentCount++;
                IndexCatalog::IndexIterator i = indexCatalog.getIndexIterator(txn, false);
                RecordId documentRecordId = document->id;

                while (i.more()) {
                    const IndexDescriptor* descriptor = i.next();
                    string indexNs = descriptor->indexNamespace();
                    ValidateResults& results = indexDetailNsResultsMap[indexNs];
                    if (!results.valid) {
                        // No point doing additional validation if the index is already invalid.
                        continue;
                    }

                    IndexAccessMethod* iam = indexCatalog.getIndex(descriptor);
                    invariant(iam);
                    BSONObj documentData = document->data.toBson();

                    if (descriptor->isPartial()) {
                        const IndexCatalogEntry* ice = indexCatalog.getEntry(descriptor);
                        if (!ice->getFilterExpression()->matchesBSON(documentData)) {
                            continue;
                        }
                    }
                    BSONObjSet documentKeySet;
                    iam->getKeys(documentData, &documentKeySet);

                    if (documentKeySet.size() > 1) {
                        if (!descriptor->isMultikey(txn)) {
                            string msg = str::stream() << "Index " << descriptor->indexName()
                                                       << " is not multi-key but has more than one"
                                                       << " key in one or more document(s)";
                            results.errors.push_back(msg);
                            results.valid = false;
                        }
                    }

                    for (const auto& key : documentKeySet) {
                        if (key.objsize() >= IndexKeyMaxSize) {
                            // Index keys >= 1024 bytes are not indexed.
                            _longKeys[indexNs]++;
                            continue;
                        }
                        uint32_t indexEntryHash = hashIndexEntry(key, documentRecordId);
                        if (_ikc->find(indexEntryHash) != _ikc->end()) {
                            (*_ikc)[indexEntryHash]--;

                            dassert((*_ikc)[indexEntryHash] >= 0);
                            if ((*_ikc)[indexEntryHash] == 0) {
                                _ikc->erase(indexEntryHash);
                            }
                        } else {
                            allIndexesValid = false;
                            results.valid = false;
                        }
                    }
                }
            }

            if (!_ikc->empty()) {
                allIndexesValid = false;
                for (auto& it : indexDetailNsResultsMap) {
                    // Marking all indexes as invalid since we don't know which one failed.
                    ValidateResults& r = it.second;
                    r.valid = false;
                }
            }
        }

        // Validate Index Key Count.
        if (allIndexesValid) {
            IndexCatalog::IndexIterator i = indexCatalog.getIndexIterator(txn, false);
            while (i.more()) {
                IndexDescriptor* descriptor = i.next();
                ValidateResults& results = indexDetailNsResultsMap[descriptor->indexNamespace()];

                if (results.valid) {
                    validateIndexKeyCount(txn, descriptor, rs->numRecords(txn), &results);
                }
                allIndexesValid = results.valid ? allIndexesValid : false;
            }
        }

        if (!allIndexesValid) {
            string msg = "One or more indexes contain invalid index entries.";
            globalResults->errors.push_back(msg);
        }
    }

private:
    bool _full;
    std::unique_ptr<IndexKeyCountMap> _ikc;
    std::map<string, long long> _longKeys;
    std::map<string, long long> _keyCounts;

    void validateIndexKeyCount(OperationContext* txn,
                               IndexDescriptor* idx,
                               int64_t numRecs,
                               ValidateResults* results) {
        string indexNs = idx->indexNamespace();
        long long numIndexedKeys = _keyCounts[indexNs];
        long long numLongKeys = _longKeys[indexNs];
        auto totalKeys = numLongKeys + numIndexedKeys;

        bool hasTooFewKeys = false;
        if (idx->isIdIndex() && totalKeys != numRecs) {
            hasTooFewKeys = totalKeys < numRecs ? true : hasTooFewKeys;
            string msg = str::stream() << "number of _id index entries (" << numIndexedKeys
                                       << ") does not match the number of documents in the index ("
                                       << numRecs - numLongKeys << ")";
            if (!failIndexKeyTooLong && !_full && (numIndexedKeys < numRecs)) {
                results->warnings.push_back(msg);
            } else {
                results->errors.push_back(msg);
                results->valid = false;
            }
        }

        if (results->valid && !idx->isMultikey(txn) && totalKeys > numRecs) {
            string err = str::stream()
                << "index " << idx->indexName() << " is not multi-key, but has more entries ("
                << numIndexedKeys << ") than documents in the index (" << numRecs - numLongKeys
                << ")";
            results->errors.push_back(err);
            results->valid = false;
        }
        // Ensure normal indexes have at least as many keys as the number of documents in the
        // collection.
        if (results->valid && !idx->isSparse() && !idx->isPartial() && !idx->isIdIndex() &&
            idx->getAccessMethodName() == "" && totalKeys < numRecs) {
            hasTooFewKeys = true;
            string msg = str::stream() << "index " << idx->indexName()
                                       << " is not sparse or partial, but has fewer entries ("
                                       << numIndexedKeys << ") than documents in the index ("
                                       << numRecs - numLongKeys << ")";
            if (!failIndexKeyTooLong && !_full) {
                results->warnings.push_back(msg);
            } else {
                results->errors.push_back(msg);
                results->valid = false;
            }
        }

        if (!_full && hasTooFewKeys) {
            string warning = str::stream()
                << "index " << idx->indexName()
                << " has fewer keys than records. This may be the result of currently or "
                   "previously running the server with the failIndexKeyTooLong parameter set to "
                   "false. Please re-run the validate command with {full: true}";
            results->warnings.push_back(warning);
        }
    }

    uint32_t hashIndexEntry(const BSONObj& key, RecordId& loc) {
        uint32_t hash;
        KeyString ks(key, Ordering::make(BSONObj()), loc);
        MurmurHash3_x86_32(ks.getTypeBits().getBuffer(), ks.getTypeBits().getSize(), 0, &hash);
        MurmurHash3_x86_32(ks.getBuffer(), ks.getSize(), hash, &hash);
        // Take the first 25 bits to conserve memory.
        return hash >> 7;
    }
};
}  // namespace

Status Collection::validate(OperationContext* txn,
                            bool full,
                            bool scanData,
                            ValidateResults* results,
                            BSONObjBuilder* output) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IS));

    RecordDataValidateAdaptor adaptor;
    Status status = _recordStore->validate(txn, full, scanData, &adaptor, results, output);
    if (!status.isOK())
        return status;

    {  // indexes
        int idxn = 0;
        try {
            // Only applicable when 'full' validation is requested.
            std::unique_ptr<BSONObjBuilder> indexDetails(full ? new BSONObjBuilder() : NULL);
            std::unique_ptr<IndexValidator> indexValidator(new IndexValidator(full));
            ValidateResultsMap indexDetailNsResultsMap;
            BSONObjBuilder keysPerIndex;  // not using subObjStart to be exception safe

            IndexCatalog::IndexIterator i = _indexCatalog.getIndexIterator(txn, false);
            while (i.more()) {
                txn->checkForInterrupt();
                const IndexDescriptor* descriptor = i.next();
                log(LogComponent::kIndex) << "validating index " << descriptor->indexNamespace()
                                          << endl;
                IndexAccessMethod* iam = _indexCatalog.getIndex(descriptor);
                invariant(iam);

                ValidateResults curIndexResults;
                int64_t numKeys;
                iam->validate(txn, full, &numKeys, &curIndexResults);
                keysPerIndex.appendNumber(descriptor->indexNamespace(),
                                          static_cast<long long>(numKeys));

                indexDetailNsResultsMap[descriptor->indexNamespace()] = curIndexResults;
                if (curIndexResults.valid) {
                    indexValidator->cacheIndexInfo(txn, iam, descriptor, numKeys);
                }

                idxn++;
            }

            indexValidator->validateIndexes(
                txn, _recordStore, _indexCatalog, indexDetailNsResultsMap, results);

            for (auto& it : indexDetailNsResultsMap) {
                std::string indexNs = it.first;
                ValidateResults& vr = it.second;

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

                results->warnings.insert(
                    results->warnings.end(), vr.warnings.begin(), vr.warnings.end());
                results->errors.insert(results->errors.end(), vr.errors.begin(), vr.errors.end());

                if (!vr.valid) {
                    results->valid = false;
                }
            }

            output->append("nIndexes", _indexCatalog.numIndexesReady(txn));
            output->append("keysPerIndex", keysPerIndex.done());
            if (indexDetails.get()) {
                output->append("indexDetails", indexDetails->done());
            }
        } catch (DBException& e) {
            if (ErrorCodes::isInterruption(ErrorCodes::Error(e.getCode()))) {
                return e.toStatus();
            }
            string err = str::stream() << "exception during index validate idxn "
                                       << BSONObjBuilder::numStr(idxn) << ": " << e.toString();
            results->errors.push_back(err);
            results->valid = false;
        }
    }

    return Status::OK();
}

Status Collection::touch(OperationContext* txn,
                         bool touchData,
                         bool touchIndexes,
                         BSONObjBuilder* output) const {
    if (touchData) {
        BSONObjBuilder b;
        Status status = _recordStore->touch(txn, &b);
        if (!status.isOK())
            return status;
        output->append("data", b.obj());
    }

    if (touchIndexes) {
        Timer t;
        IndexCatalog::IndexIterator ii = _indexCatalog.getIndexIterator(txn, false);
        while (ii.more()) {
            const IndexDescriptor* desc = ii.next();
            const IndexAccessMethod* iam = _indexCatalog.getIndex(desc);
            Status status = iam->touch(txn);
            if (!status.isOK())
                return status;
        }

        output->append("indexes",
                       BSON("num" << _indexCatalog.numIndexesTotal(txn) << "millis" << t.millis()));
    }

    return Status::OK();
}
}
