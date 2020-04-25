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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_consistency_markers_impl.h"

#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace repl {

constexpr StringData ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace;
constexpr StringData ReplicationConsistencyMarkersImpl::kDefaultOplogTruncateAfterPointNamespace;
constexpr StringData ReplicationConsistencyMarkersImpl::kDefaultInitialSyncIdNamespace;

namespace {
const BSONObj kInitialSyncFlag(BSON(MinValidDocument::kInitialSyncFlagFieldName << true));
const BSONObj kOplogTruncateAfterPointId(BSON("_id"
                                              << "oplogTruncateAfterPoint"));
}  // namespace

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface)
    : ReplicationConsistencyMarkersImpl(
          storageInterface,
          NamespaceString(ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace),
          NamespaceString(
              ReplicationConsistencyMarkersImpl::kDefaultOplogTruncateAfterPointNamespace),
          NamespaceString(ReplicationConsistencyMarkersImpl::kDefaultInitialSyncIdNamespace)) {}

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface,
    NamespaceString minValidNss,
    NamespaceString oplogTruncateAfterPointNss,
    NamespaceString initialSyncIdNss)
    : _storageInterface(storageInterface),
      _minValidNss(minValidNss),
      _oplogTruncateAfterPointNss(oplogTruncateAfterPointNss),
      _initialSyncIdNss(initialSyncIdNss) {}

boost::optional<MinValidDocument> ReplicationConsistencyMarkersImpl::_getMinValidDocument(
    OperationContext* opCtx) const {
    auto result = _storageInterface->findSingleton(opCtx, _minValidNss);
    if (!result.isOK()) {
        if (result.getStatus() == ErrorCodes::NamespaceNotFound ||
            result.getStatus() == ErrorCodes::CollectionIsEmpty) {
            return boost::none;
        }
        // Fail if there is an error other than the collection being missing or being empty.
        fassertFailedWithStatus(40466, result.getStatus());
    }

    auto minValid =
        MinValidDocument::parse(IDLParserErrorContext("MinValidDocument"), result.getValue());
    return minValid;
}

void ReplicationConsistencyMarkersImpl::_updateMinValidDocument(
    OperationContext* opCtx, const TimestampedBSONObj& updateSpec) {
    Status status = _storageInterface->putSingleton(opCtx, _minValidNss, updateSpec);
    invariant(status);
}

void ReplicationConsistencyMarkersImpl::initializeMinValidDocument(OperationContext* opCtx) {
    LOGV2_DEBUG(21282, 3, "Initializing minValid document");

    // This initializes the values of the required fields if they are not already set.
    // If one of the fields is already set, the $max will prefer the existing value since it
    // will always be greater than the provided ones.
    TimestampedBSONObj upsert;
    upsert.obj = BSON("$max" << BSON(MinValidDocument::kMinValidTimestampFieldName
                                     << Timestamp() << MinValidDocument::kMinValidTermFieldName
                                     << OpTime::kUninitializedTerm));

    // The initialization write should go into the first checkpoint taken, so we provide no
    // timestamp. The 'minValid' document could exist already and this could simply add fields to
    // the 'minValid' document, but we still want the initialization write to go into the next
    // checkpoint since a newly initialized 'minValid' document is always valid.
    upsert.timestamp = Timestamp();
    fassert(40467, _storageInterface->putSingleton(opCtx, _minValidNss, upsert));
}

bool ReplicationConsistencyMarkersImpl::getInitialSyncFlag(OperationContext* opCtx) const {
    auto doc = _getMinValidDocument(opCtx);
    if (!doc) {
        LOGV2_DEBUG(
            21283, 3, "No min valid document found, returning initial sync flag value of false");
        return false;
    }

    boost::optional<bool> flag = doc->getInitialSyncFlag();
    if (!flag) {
        LOGV2_DEBUG(
            21284, 3, "No initial sync flag set, returning initial sync flag value of false");
        return false;
    }

    LOGV2_DEBUG(21285,
                3,
                "returning initial sync flag value of {flag}",
                "Returning initial sync flag value",
                "flag"_attr = flag.get());
    return flag.get();
}

void ReplicationConsistencyMarkersImpl::setInitialSyncFlag(OperationContext* opCtx) {
    LOGV2_DEBUG(21286, 3, "Setting initial sync flag");
    TimestampedBSONObj update;
    update.obj = BSON("$set" << kInitialSyncFlag);

    // We do not provide a timestamp when we set the initial sync flag. Initial sync can only
    // occur right when we start up, and thus there cannot be any checkpoints being taken. This
    // write should go into the next checkpoint.
    update.timestamp = Timestamp();

    _updateMinValidDocument(opCtx, update);
    opCtx->recoveryUnit()->waitUntilDurable(opCtx);
}

void ReplicationConsistencyMarkersImpl::clearInitialSyncFlag(OperationContext* opCtx) {
    LOGV2_DEBUG(21287, 3, "Clearing initial sync flag");

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    OpTimeAndWallTime opTimeAndWallTime = replCoord->getMyLastAppliedOpTimeAndWallTime();
    const auto time = opTimeAndWallTime.opTime;
    TimestampedBSONObj update;
    update.obj = BSON("$unset" << kInitialSyncFlag << "$set"
                               << BSON(MinValidDocument::kMinValidTimestampFieldName
                                       << time.getTimestamp()
                                       << MinValidDocument::kMinValidTermFieldName << time.getTerm()
                                       << MinValidDocument::kAppliedThroughFieldName << time));

    // As we haven't yet updated our initialDataTimestamp from
    // Timestamp::kAllowUnstableCheckpointsSentinel to lastAppliedTimestamp, we are only allowed to
    // take unstable checkpoints. And, this "lastAppliedTimestamp" will be the first stable
    // checkpoint taken after initial sync. So, no way this minValid update can be part of a stable
    // checkpoint taken earlier than lastAppliedTimestamp. So, it's safe to make it as an
    // non-timestamped write. Also, this has to be non-timestamped write because we may have readers
    // at lastAppliedTimestamp, commiting the storage writes before or at such timestamps is
    // illegal.
    update.timestamp = Timestamp();

    _updateMinValidDocument(opCtx, update);

    // Make sure to clear the oplogTrucateAfterPoint in case it is stale. Otherwise, we risk the
    // possibility of deleting oplog entries that we want to keep. It is safe to clear this
    // here since we are consistent at the top of our oplog at this point.
    invariant(!isOplogTruncateAfterPointBeingUsedForPrimary(),
              "Clearing the truncate point while primary is unsafe: it is asynchronously updated.");
    setOplogTruncateAfterPoint(opCtx, Timestamp());

    if (getGlobalServiceContext()->getStorageEngine()->isDurable()) {
        opCtx->recoveryUnit()->waitUntilDurable(opCtx);
        replCoord->setMyLastDurableOpTimeAndWallTime(opTimeAndWallTime);
    }
}

OpTime ReplicationConsistencyMarkersImpl::getMinValid(OperationContext* opCtx) const {
    auto doc = _getMinValidDocument(opCtx);
    invariant(doc);  // Initialized at startup so it should never be missing.

    auto minValid = OpTime(doc->getMinValidTimestamp(), doc->getMinValidTerm());

    LOGV2_DEBUG(21288,
                3,
                "returning minvalid: {minValidString}({minValidBSON})",
                "Returning minvalid",
                "minValidString"_attr = minValid.toString(),
                "minValidBSON"_attr = minValid.toBSON());

    return minValid;
}

void ReplicationConsistencyMarkersImpl::setMinValid(OperationContext* opCtx,
                                                    const OpTime& minValid) {
    LOGV2_DEBUG(21289,
                3,
                "setting minvalid to exactly: {minValidString}({minValidBSON})",
                "Setting minvalid to exactly",
                "minValidString"_attr = minValid.toString(),
                "minValidBSON"_attr = minValid.toBSON());
    TimestampedBSONObj update;
    update.obj =
        BSON("$set" << BSON(MinValidDocument::kMinValidTimestampFieldName
                            << minValid.getTimestamp() << MinValidDocument::kMinValidTermFieldName
                            << minValid.getTerm()));

    // This method is only used with storage engines that do not support recover to stable
    // timestamp. As a result, their timestamps do not matter.
    invariant(!opCtx->getServiceContext()->getStorageEngine()->supportsRecoverToStableTimestamp());
    update.timestamp = Timestamp();

    _updateMinValidDocument(opCtx, update);
}

void ReplicationConsistencyMarkersImpl::setMinValidToAtLeast(OperationContext* opCtx,
                                                             const OpTime& minValid) {
    LOGV2_DEBUG(21290,
                3,
                "setting minvalid to at least: {minValidString}({minValidBSON})",
                "Setting minvalid to at least",
                "minValidString"_attr = minValid.toString(),
                "minValidBSON"_attr = minValid.toBSON());

    auto& termField = MinValidDocument::kMinValidTermFieldName;
    auto& tsField = MinValidDocument::kMinValidTimestampFieldName;

    // Always update both fields of optime.
    auto updateSpec =
        BSON("$set" << BSON(tsField << minValid.getTimestamp() << termField << minValid.getTerm()));
    BSONObj query;
    if (minValid.getTerm() == OpTime::kUninitializedTerm) {
        // Only compare timestamps in PV0, but update both fields of optime.
        // e.g { ts: { $lt: Timestamp 1508961481000|2 } }
        query = BSON(tsField << LT << minValid.getTimestamp());
    } else {
        // Set the minValid only if the given term is higher or the terms are the same but
        // the given timestamp is higher.
        // e.g. { $or: [ { t: { $lt: 1 } }, { t: 1, ts: { $lt: Timestamp 1508961481000|6 } } ] }
        query = BSON(
            OR(BSON(termField << LT << minValid.getTerm()),
               BSON(termField << minValid.getTerm() << tsField << LT << minValid.getTimestamp())));
    }

    TimestampedBSONObj update;
    update.obj = updateSpec;

    // We write to the 'minValid' document with the 'minValid' timestamp. We only take stable
    // checkpoints when we are consistent. Thus, the next checkpoint we can take is at this
    // 'minValid'. If we gave it a timestamp from before the batch, and we took a stable checkpoint
    // at that timestamp, then we would consider that checkpoint inconsistent, even though it is
    // consistent.
    update.timestamp = minValid.getTimestamp();

    Status status = _storageInterface->updateSingleton(opCtx, _minValidNss, query, update);
    invariant(status);
}

void ReplicationConsistencyMarkersImpl::setAppliedThrough(OperationContext* opCtx,
                                                          const OpTime& optime,
                                                          bool setTimestamp) {
    invariant(!optime.isNull());
    LOGV2_DEBUG(21291,
                3,
                "setting appliedThrough to: {appliedThroughString}({appliedThroughBSON})",
                "Setting appliedThrough",
                "appliedThroughString"_attr = optime.toString(),
                "appliedThroughBSON"_attr = optime.toBSON());

    // We set the 'appliedThrough' to the provided timestamp. The 'appliedThrough' is only valid
    // in checkpoints that contain all writes through this timestamp since it indicates the top of
    // the oplog.
    TimestampedBSONObj update;
    if (setTimestamp) {
        update.timestamp = optime.getTimestamp();
    }
    update.obj = BSON("$set" << BSON(MinValidDocument::kAppliedThroughFieldName << optime));

    _updateMinValidDocument(opCtx, update);
}

void ReplicationConsistencyMarkersImpl::clearAppliedThrough(OperationContext* opCtx,
                                                            const Timestamp& writeTimestamp) {
    LOGV2_DEBUG(21292,
                3,
                "clearing appliedThrough at: {writeTimestamp}",
                "Clearing appliedThrough",
                "writeTimestamp"_attr = writeTimestamp.toString());

    TimestampedBSONObj update;
    update.timestamp = writeTimestamp;
    update.obj = BSON("$unset" << BSON(MinValidDocument::kAppliedThroughFieldName << 1));

    _updateMinValidDocument(opCtx, update);
}

OpTime ReplicationConsistencyMarkersImpl::getAppliedThrough(OperationContext* opCtx) const {
    auto doc = _getMinValidDocument(opCtx);
    invariant(doc);  // Initialized at startup so it should never be missing.

    auto appliedThrough = doc->getAppliedThrough();
    if (!appliedThrough) {
        LOGV2_DEBUG(
            21293, 3, "No appliedThrough OpTime set, returning empty appliedThrough OpTime");
        return {};
    }
    LOGV2_DEBUG(21294,
                3,
                "returning appliedThrough: {appliedThroughString}({appliedThroughBSON})",
                "Returning appliedThrough",
                "appliedThroughString"_attr = appliedThrough->toString(),
                "appliedThroughBSON"_attr = appliedThrough->toBSON());

    return appliedThrough.get();
}

void ReplicationConsistencyMarkersImpl::ensureFastCountOnOplogTruncateAfterPoint(
    OperationContext* opCtx) {
    LOGV2_DEBUG(
        21295,
        3,
        "Updating cached fast-count on collection {oplogTruncateAfterPointNamespace} in case an "
        "unclean shutdown caused it to become incorrect.",
        "Updating cached fast-count on oplog truncate after point collection in case an unclean "
        "shutdown caused it to become incorrect",
        "oplogTruncateAfterPointNamespace"_attr = _oplogTruncateAfterPointNss);

    auto result = _storageInterface->findSingleton(opCtx, _oplogTruncateAfterPointNss);

    if (result.getStatus() == ErrorCodes::NamespaceNotFound) {
        return;
    }

    if (result.getStatus() == ErrorCodes::CollectionIsEmpty) {
        // The count is updated before successful commit of a write, so unclean shutdown can leave
        // the value incorrectly set to one.
        invariant(
            _storageInterface->setCollectionCount(opCtx, _oplogTruncateAfterPointNss, 0).isOK());
        return;
    }

    if (result.getStatus() == ErrorCodes::TooManyMatchingDocuments) {
        fassert(51265,
                {result.getStatus().code(),
                 str::stream() << "More than one document was found in the '"
                               << kDefaultOplogTruncateAfterPointNamespace
                               << "' collection. Users should not write to this collection. Please "
                                  "delete the excess documents"});
    }
    fassert(51266, result.getStatus());

    // We can safely set a count of one. We know that we only ever write one document, and the
    // success of findSingleton above confirms only one document exists in the collection.
    invariant(_storageInterface->setCollectionCount(opCtx, _oplogTruncateAfterPointNss, 1).isOK());
}

void ReplicationConsistencyMarkersImpl::_upsertOplogTruncateAfterPointDocument(
    OperationContext* opCtx, const BSONObj& updateSpec) {
    fassert(40512,
            _storageInterface->upsertById(
                opCtx, _oplogTruncateAfterPointNss, kOplogTruncateAfterPointId["_id"], updateSpec));
}

void ReplicationConsistencyMarkersImpl::setOplogTruncateAfterPoint(OperationContext* opCtx,
                                                                   const Timestamp& timestamp) {
    LOGV2_DEBUG(21296,
                3,
                "setting oplog truncate after point to: {oplogTruncateAfterPoint}",
                "Setting oplog truncate after point",
                "oplogTruncateAfterPoint"_attr = timestamp.toBSON());
    _upsertOplogTruncateAfterPointDocument(
        opCtx,
        BSON("$set" << BSON(OplogTruncateAfterPointDocument::kOplogTruncateAfterPointFieldName
                            << timestamp)));
}

boost::optional<OplogTruncateAfterPointDocument>
ReplicationConsistencyMarkersImpl::_getOplogTruncateAfterPointDocument(
    OperationContext* opCtx) const {
    auto doc = _storageInterface->findById(
        opCtx, _oplogTruncateAfterPointNss, kOplogTruncateAfterPointId["_id"]);

    if (!doc.isOK()) {
        if (doc.getStatus() == ErrorCodes::NoSuchKey ||
            doc.getStatus() == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        } else {
            // Fails if there is an error other than the collection being missing or being empty.
            fassertFailedWithStatus(40510, doc.getStatus());
        }
    }

    auto oplogTruncateAfterPoint = OplogTruncateAfterPointDocument::parse(
        IDLParserErrorContext("OplogTruncateAfterPointDocument"), doc.getValue());
    return oplogTruncateAfterPoint;
}

Timestamp ReplicationConsistencyMarkersImpl::getOplogTruncateAfterPoint(
    OperationContext* opCtx) const {
    auto truncatePointDoc = _getOplogTruncateAfterPointDocument(opCtx);
    if (!truncatePointDoc) {
        LOGV2_DEBUG(
            21297, 3, "Returning empty oplog truncate after point since document did not exist");
        return Timestamp();
    }
    Timestamp truncatePointTimestamp = truncatePointDoc->getOplogTruncateAfterPoint();

    LOGV2_DEBUG(21298,
                3,
                "Returning oplog truncate after point: {oplogTruncateAfterPoint}",
                "Returning oplog truncate after point",
                "oplogTruncateAfterPoint"_attr = truncatePointTimestamp);
    return truncatePointTimestamp;
}

void ReplicationConsistencyMarkersImpl::startUsingOplogTruncateAfterPointForPrimary() {
    stdx::lock_guard<Latch> lk(_truncatePointIsPrimaryMutex);
    // There is only one path to stepup and it is not called redundantly.
    invariant(!_isPrimary);
    _isPrimary = true;
}

void ReplicationConsistencyMarkersImpl::stopUsingOplogTruncateAfterPointForPrimary() {
    stdx::lock_guard<Latch> lk(_truncatePointIsPrimaryMutex);
    _isPrimary = false;
}

bool ReplicationConsistencyMarkersImpl::isOplogTruncateAfterPointBeingUsedForPrimary() const {
    stdx::lock_guard<Latch> lk(_truncatePointIsPrimaryMutex);
    return _isPrimary;
}

void ReplicationConsistencyMarkersImpl::setOplogTruncateAfterPointToTopOfOplog(
    OperationContext* opCtx) {
    auto timestamp = _storageInterface->getLatestOplogTimestamp(opCtx);
    LOGV2_DEBUG(21551,
                3,
                "Initializing oplog truncate after point: {oplogTruncateAfterPoint}",
                "Initializing oplog truncate after point",
                "oplogTruncateAfterPoint"_attr = timestamp);
    setOplogTruncateAfterPoint(opCtx, timestamp);
}

boost::optional<OpTimeAndWallTime>
ReplicationConsistencyMarkersImpl::refreshOplogTruncateAfterPointIfPrimary(
    OperationContext* opCtx) {

    if (!isOplogTruncateAfterPointBeingUsedForPrimary()) {
        // Stepdown clears the truncate point, after which the truncate point is set manually as
        // needed, so nothing should be done here -- else we might truncate something we should not.
        return boost::none;
    }

    // Temporarily allow writes if kIgnoreConflicts is set on the recovery unit so the truncate
    // point can be updated. The kIgnoreConflicts setting only allows reads.
    auto originalBehavior = opCtx->recoveryUnit()->getPrepareConflictBehavior();
    if (originalBehavior == PrepareConflictBehavior::kIgnoreConflicts) {
        opCtx->recoveryUnit()->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
    }
    ON_BLOCK_EXIT([&] { opCtx->recoveryUnit()->setPrepareConflictBehavior(originalBehavior); });

    // The locks necessary to write to the oplog truncate after point's collection and read from the
    // oplog collection must be taken up front so that the mutex can also be taken around both
    // operations without causing deadlocks.
    AutoGetCollection autoTruncateColl(opCtx, _oplogTruncateAfterPointNss, MODE_IX);
    AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
    stdx::lock_guard<Latch> lk(_refreshOplogTruncateAfterPointMutex);

    // Update the oplogTruncateAfterPoint to the storage engine's reported oplog timestamp with no
    // holes behind it in-memory (only, not on disk, despite the name).
    auto truncateTimestamp = _storageInterface->getAllDurableTimestamp(opCtx->getServiceContext());

    if (truncateTimestamp != Timestamp(StorageEngine::kMinimumTimestamp)) {
        setOplogTruncateAfterPoint(opCtx, truncateTimestamp);
    } else {
        // The all_durable timestamp has not yet been set: there have been no oplog writes since
        // this server instance started up. In this case, we will return the current
        // oplogTruncateAfterPoint without updating it, since there's nothing to update.
        truncateTimestamp = getOplogTruncateAfterPoint(opCtx);

        // A primary cannot have an unset oplogTruncateAfterPoint because it is initialized on
        // step-up.
        invariant(!truncateTimestamp.isNull());
    }

    // Reset the snapshot so that it is ensured to see the latest oplog entries.
    opCtx->recoveryUnit()->abandonSnapshot();

    // Fetch the oplog entry <= timestamp. all_durable may be set to a value between oplog entries.
    // We need an oplog entry in order to return term and wallclock time for an OpTimeAndWallTime
    // result.
    auto truncateOplogEntryBSON = _storageInterface->findOplogEntryLessThanOrEqualToTimestamp(
        opCtx, oplogRead.getCollection(), truncateTimestamp);

    // The truncate point moves the Durable timestamp forward, so it should always exist in the
    // oplog.
    invariant(truncateOplogEntryBSON, "Found no oplog entry lte " + truncateTimestamp.toString());

    return fassert(
        44555001,
        OpTimeAndWallTime::parseOpTimeAndWallTimeFromOplogEntry(truncateOplogEntryBSON.get()));
}

Status ReplicationConsistencyMarkersImpl::createInternalCollections(OperationContext* opCtx) {
    for (auto nss : std::vector<NamespaceString>({_oplogTruncateAfterPointNss, _minValidNss})) {
        auto status = _storageInterface->createCollection(opCtx, nss, CollectionOptions());
        if (!status.isOK() && status.code() != ErrorCodes::NamespaceExists) {
            return {ErrorCodes::CannotCreateCollection,
                    str::stream() << "Failed to create collection. Ns: " << nss.ns()
                                  << " Error: " << status.toString()};
        }
    }
    return Status::OK();
}

void ReplicationConsistencyMarkersImpl::setInitialSyncIdIfNotSet(OperationContext* opCtx) {
    auto status =
        _storageInterface->createCollection(opCtx, _initialSyncIdNss, CollectionOptions());
    if (!status.isOK() && status.code() != ErrorCodes::NamespaceExists) {
        LOGV2_FATAL(
            4608500, "Failed to create collection", "namespace"_attr = _initialSyncIdNss.ns());
        fassertFailedWithStatus(4608502, status);
    }

    auto prevId = _storageInterface->findSingleton(opCtx, _initialSyncIdNss);
    if (prevId.getStatus() == ErrorCodes::CollectionIsEmpty) {
        auto doc = BSON("_id" << UUID::gen() << "wallTime"
                              << opCtx->getServiceContext()->getPreciseClockSource()->now());
        fassert(4608503,
                _storageInterface->insertDocument(opCtx,
                                                  _initialSyncIdNss,
                                                  TimestampedBSONObj{doc, Timestamp()},
                                                  OpTime::kUninitializedTerm));
    } else if (!prevId.isOK()) {
        fassertFailedWithStatus(4608504, prevId.getStatus());
    }
}

void ReplicationConsistencyMarkersImpl::clearInitialSyncId(OperationContext* opCtx) {
    fassert(4608501, _storageInterface->dropCollection(opCtx, _initialSyncIdNss));
}

BSONObj ReplicationConsistencyMarkersImpl::getInitialSyncId(OperationContext* opCtx) {
    auto idStatus = _storageInterface->findSingleton(opCtx, _initialSyncIdNss);
    if (idStatus.isOK()) {
        return idStatus.getValue();
    }
    if (idStatus.getStatus() != ErrorCodes::CollectionIsEmpty &&
        idStatus.getStatus() != ErrorCodes::NamespaceNotFound) {
        uassertStatusOK(idStatus);
    }
    return BSONObj();
}

}  // namespace repl
}  // namespace mongo
