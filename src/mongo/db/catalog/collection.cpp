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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"


#include "mongo/base/counter.h"
#include "mongo/base/owned_pointer_map.h"
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
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/record_fetcher.h"

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

CappedInsertNotifier::CappedInsertNotifier() : _cappedInsertCount(0) {}

void CappedInsertNotifier::notifyOfInsert() {
    stdx::lock_guard<stdx::mutex> lk(_cappedNewDataMutex);
    _cappedInsertCount++;
    _cappedNewDataNotifier.notify_all();
}

uint64_t CappedInsertNotifier::getCount() const {
    stdx::lock_guard<stdx::mutex> lk(_cappedNewDataMutex);
    return _cappedInsertCount;
}

void CappedInsertNotifier::waitForInsert(uint64_t referenceCount, Microseconds timeout) const {
    stdx::unique_lock<stdx::mutex> lk(_cappedNewDataMutex);

    while (referenceCount == _cappedInsertCount) {
        if (stdx::cv_status::timeout == _cappedNewDataNotifier.wait_for(lk, timeout)) {
            return;
        }
    }
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
        _recordStore->setCappedDeleteCallback(this);
    _infoCache.reset(txn);
}

Collection::~Collection() {
    verify(ok());
    _magic = 0;
}

bool Collection::requiresIdIndex() const {
    if (_ns.ns().find('$') != string::npos) {
        // no indexes on indexes
        return false;
    }

    if (_ns.isSystem()) {
        StringData shortName = _ns.coll().substr(_ns.coll().find('.') + 1);
        if (shortName == "indexes" || shortName == "namespaces" || shortName == "profile") {
            return false;
        }
    }

    if (_ns.db() == "local") {
        if (_ns.coll().startsWith("oplog."))
            return false;
    }

    if (!_ns.isSystem()) {
        // non system collections definitely have an _id index
        return true;
    }


    return true;
}

std::unique_ptr<RecordCursor> Collection::getCursor(OperationContext* txn, bool forward) const {
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

    auto statusWithMatcher = MatchExpressionParser::parse(validator);
    if (!statusWithMatcher.isOK())
        return statusWithMatcher.getStatus();

    return statusWithMatcher;
}

StatusWith<RecordId> Collection::insertDocument(OperationContext* txn,
                                                const DocWriter* doc,
                                                bool enforceQuota) {
    invariant(!_validator || documentValidationDisabled(txn));
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));
    invariant(!_indexCatalog.haveAnyIndexes());  // eventually can implement, just not done

    if (_mustTakeCappedLockOnInsert)
        synchronizeOnCappedInFlightResource(txn->lockState());

    StatusWith<RecordId> loc = _recordStore->insertRecord(txn, doc, _enforceQuota(enforceQuota));
    if (!loc.isOK())
        return loc;

    // we cannot call into the OpObserver here because the document being written is not present
    // fortunately, this is currently only used for adding entries to the oplog.

    // If there is a notifier object and another thread is waiting on it, then we notify waiters
    // of this document insert. Waiters keep a shared_ptr to '_cappedNotifier', so there are
    // waiters if this Collection's shared_ptr is not unique.
    if (_cappedNotifier && !_cappedNotifier.unique()) {
        _cappedNotifier->notifyOfInsert();
    }

    return StatusWith<RecordId>(loc);
}

StatusWith<RecordId> Collection::insertDocument(OperationContext* txn,
                                                const BSONObj& docToInsert,
                                                bool enforceQuota,
                                                bool fromMigrate) {
    {
        auto status = checkValidation(txn, docToInsert);
        if (!status.isOK())
            return status;
    }

    const SnapshotId sid = txn->recoveryUnit()->getSnapshotId();

    if (_indexCatalog.findIdIndex(txn)) {
        if (docToInsert["_id"].eoo()) {
            return StatusWith<RecordId>(ErrorCodes::InternalError,
                                        str::stream()
                                            << "Collection::insertDocument got "
                                               "document without _id for ns:" << _ns.ns());
        }
    }

    if (_mustTakeCappedLockOnInsert)
        synchronizeOnCappedInFlightResource(txn->lockState());

    StatusWith<RecordId> res = _insertDocument(txn, docToInsert, enforceQuota);
    invariant(sid == txn->recoveryUnit()->getSnapshotId());
    if (res.isOK()) {
        getGlobalServiceContext()->getOpObserver()->onInsert(txn, ns(), docToInsert, fromMigrate);

        // If there is a notifier object and another thread is waiting on it, then we notify
        // waiters of this document insert. Waiters keep a shared_ptr to '_cappedNotifier', so
        // there are waiters if this Collection's shared_ptr is not unique.
        if (_cappedNotifier && !_cappedNotifier.unique()) {
            _cappedNotifier->notifyOfInsert();
        }
    }

    return res;
}

StatusWith<RecordId> Collection::insertDocument(OperationContext* txn,
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
        synchronizeOnCappedInFlightResource(txn->lockState());

    StatusWith<RecordId> loc =
        _recordStore->insertRecord(txn, doc.objdata(), doc.objsize(), _enforceQuota(enforceQuota));

    if (!loc.isOK())
        return loc;

    Status status = indexBlock->insert(doc, loc.getValue());
    if (!status.isOK())
        return StatusWith<RecordId>(status);

    getGlobalServiceContext()->getOpObserver()->onInsert(txn, ns(), doc);

    // If there is a notifier object and another thread is waiting on it, then we notify waiters
    // of this document insert. Waiters keep a shared_ptr to '_cappedNotifier', so there are
    // waiters if this Collection's shared_ptr is not unique.
    if (_cappedNotifier && !_cappedNotifier.unique()) {
        _cappedNotifier->notifyOfInsert();
    }

    return loc;
}

StatusWith<RecordId> Collection::_insertDocument(OperationContext* txn,
                                                 const BSONObj& docToInsert,
                                                 bool enforceQuota) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));

    // TODO: for now, capped logic lives inside NamespaceDetails, which is hidden
    //       under the RecordStore, this feels broken since that should be a
    //       collection access method probably

    StatusWith<RecordId> loc = _recordStore->insertRecord(
        txn, docToInsert.objdata(), docToInsert.objsize(), _enforceQuota(enforceQuota));
    if (!loc.isOK())
        return loc;

    invariant(RecordId::min() < loc.getValue());
    invariant(loc.getValue() < RecordId::max());

    Status s = _indexCatalog.indexRecord(txn, docToInsert, loc.getValue());
    if (!s.isOK())
        return StatusWith<RecordId>(s);

    return loc;
}

Status Collection::aboutToDeleteCapped(OperationContext* txn,
                                       const RecordId& loc,
                                       RecordData data) {
    /* check if any cursors point to us.  if so, advance them. */
    _cursorManager.invalidateDocument(txn, loc, INVALIDATION_DELETION);

    BSONObj doc = data.releaseToBson();
    _indexCatalog.unindexRecord(txn, doc, loc, false);

    return Status::OK();
}

void Collection::deleteDocument(
    OperationContext* txn, const RecordId& loc, bool cappedOK, bool noWarn, BSONObj* deletedId) {
    if (isCapped() && !cappedOK) {
        log() << "failing remove on a capped ns " << _ns << endl;
        uasserted(10089, "cannot remove from a capped collection");
        return;
    }

    Snapshotted<BSONObj> doc = docFor(txn, loc);

    BSONElement e = doc.value()["_id"];
    BSONObj id;
    if (e.type()) {
        id = e.wrap();
        if (deletedId) {
            *deletedId = e.wrap();
        }
    }

    /* check if any cursors point to us.  if so, advance them. */
    _cursorManager.invalidateDocument(txn, loc, INVALIDATION_DELETION);

    _indexCatalog.unindexRecord(txn, doc.value(), loc, noWarn);

    _recordStore->deleteRecord(txn, loc);

    if (!id.isEmpty()) {
        getGlobalServiceContext()->getOpObserver()->onDelete(txn, ns().ns(), id);
    }
}

Counter64 moveCounter;
ServerStatusMetricField<Counter64> moveCounterDisplay("record.moves", &moveCounter);

StatusWith<RecordId> Collection::updateDocument(OperationContext* txn,
                                                const RecordId& oldLocation,
                                                const Snapshotted<BSONObj>& oldDoc,
                                                const BSONObj& newDoc,
                                                bool enforceQuota,
                                                bool indexesAffected,
                                                OpDebug* debug,
                                                oplogUpdateEntryArgs& args) {
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

    SnapshotId sid = txn->recoveryUnit()->getSnapshotId();

    BSONElement oldId = oldDoc.value()["_id"];
    if (!oldId.eoo() && (oldId != newDoc["_id"]))
        return StatusWith<RecordId>(
            ErrorCodes::InternalError, "in Collection::updateDocument _id mismatch", 13596);

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

    // This can call back into Collection::recordStoreGoingToMove.  If that happens, the old
    // object is removed from all indexes.
    StatusWith<RecordId> newLocation = _recordStore->updateRecord(
        txn, oldLocation, newDoc.objdata(), newDoc.objsize(), _enforceQuota(enforceQuota), this);

    if (!newLocation.isOK()) {
        return newLocation;
    }

    // At this point, the old object may or may not still be indexed, depending on if it was
    // moved. If the object did move, we need to add the new location to all indexes.
    if (newLocation.getValue() != oldLocation) {
        if (debug) {
            if (debug->nmoved == -1)  // default of -1 rather than 0
                debug->nmoved = 1;
            else
                debug->nmoved += 1;
        }

        Status s = _indexCatalog.indexRecord(txn, newDoc, newLocation.getValue());
        if (!s.isOK())
            return StatusWith<RecordId>(s);
        invariant(sid == txn->recoveryUnit()->getSnapshotId());
        args.ns = ns().ns();
        getGlobalServiceContext()->getOpObserver()->onUpdate(txn, args);

        return newLocation;
    }

    // Object did not move.  We update each index with each respective UpdateTicket.

    if (debug)
        debug->keyUpdates = 0;

    if (indexesAffected) {
        IndexCatalog::IndexIterator ii = _indexCatalog.getIndexIterator(txn, true);
        while (ii.more()) {
            IndexDescriptor* descriptor = ii.next();
            IndexAccessMethod* iam = ii.accessMethod(descriptor);

            int64_t updatedKeys;
            Status ret = iam->update(txn, *updateTickets.mutableMap()[descriptor], &updatedKeys);
            if (!ret.isOK())
                return StatusWith<RecordId>(ret);
            if (debug)
                debug->keyUpdates += updatedKeys;
        }
    }

    invariant(sid == txn->recoveryUnit()->getSnapshotId());
    args.ns = ns().ns();
    getGlobalServiceContext()->getOpObserver()->onUpdate(txn, args);

    return newLocation;
}

Status Collection::recordStoreGoingToMove(OperationContext* txn,
                                          const RecordId& oldLocation,
                                          const char* oldBuffer,
                                          size_t oldSize) {
    moveCounter.increment();
    _cursorManager.invalidateDocument(txn, oldLocation, INVALIDATION_DELETION);
    _indexCatalog.unindexRecord(txn, BSONObj(oldBuffer), oldLocation, true);
    return Status::OK();
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

Status Collection::updateDocumentWithDamages(OperationContext* txn,
                                             const RecordId& loc,
                                             const Snapshotted<RecordData>& oldRec,
                                             const char* damageSource,
                                             const mutablebson::DamageVector& damages,
                                             oplogUpdateEntryArgs& args) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IX));
    invariant(oldRec.snapshotId() == txn->recoveryUnit()->getSnapshotId());
    invariant(updateWithDamagesSupported());

    // Broadcast the mutation so that query results stay correct.
    _cursorManager.invalidateDocument(txn, loc, INVALIDATION_MUTATION);

    Status status =
        _recordStore->updateWithDamages(txn, loc, oldRec.value(), damageSource, damages);

    if (status.isOK()) {
        args.ns = ns().ns();
        getGlobalServiceContext()->getOpObserver()->onUpdate(txn, args);
    }
    return status;
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
    massert(17445, "index build in progress", _indexCatalog.numIndexesInProgress(txn) == 0);

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
    _infoCache.reset(txn);

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
        return ERROR;
    } else if (newAction == "warn") {
        return WARN;
    } else if (newAction == "error") {
        return ERROR;
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
        case ERROR:
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
class MyValidateAdaptor : public ValidateAdaptor {
public:
    virtual ~MyValidateAdaptor() {}

    virtual Status validate(const RecordData& record, size_t* dataSize) {
        BSONObj obj = record.toBson();
        const Status status = validateBSON(obj.objdata(), obj.objsize());
        if (status.isOK())
            *dataSize = obj.objsize();
        return Status::OK();
    }
};
}

Status Collection::validate(OperationContext* txn,
                            bool full,
                            bool scanData,
                            ValidateResults* results,
                            BSONObjBuilder* output) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_IS));

    MyValidateAdaptor adaptor;
    Status status = _recordStore->validate(txn, full, scanData, &adaptor, results, output);
    if (!status.isOK())
        return status;

    {  // indexes
        output->append("nIndexes", _indexCatalog.numIndexesReady(txn));
        int idxn = 0;
        try {
            // Only applicable when 'full' validation is requested.
            std::unique_ptr<BSONObjBuilder> indexDetails(full ? new BSONObjBuilder() : NULL);
            BSONObjBuilder indexes;  // not using subObjStart to be exception safe

            IndexCatalog::IndexIterator i = _indexCatalog.getIndexIterator(txn, false);
            while (i.more()) {
                const IndexDescriptor* descriptor = i.next();
                log(LogComponent::kIndex) << "validating index " << descriptor->indexNamespace()
                                          << endl;
                IndexAccessMethod* iam = _indexCatalog.getIndex(descriptor);
                invariant(iam);

                std::unique_ptr<BSONObjBuilder> bob(
                    indexDetails.get() ? new BSONObjBuilder(indexDetails->subobjStart(
                                             descriptor->indexNamespace()))
                                       : NULL);

                int64_t keys;
                iam->validate(txn, full, &keys, bob.get());
                indexes.appendNumber(descriptor->indexNamespace(), static_cast<long long>(keys));

                if (bob) {
                    BSONObj obj = bob->done();
                    BSONElement valid = obj["valid"];
                    if (valid.ok() && !valid.trueValue()) {
                        results->valid = false;
                    }
                }
                idxn++;
            }

            output->append("keysPerIndex", indexes.done());
            if (indexDetails.get()) {
                output->append("indexDetails", indexDetails->done());
            }
        } catch (DBException& exc) {
            string err = str::stream() << "exception during index validate idxn "
                                       << BSONObjBuilder::numStr(idxn) << ": " << exc.toString();
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
