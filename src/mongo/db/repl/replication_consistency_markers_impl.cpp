/**
 *    Copyright (C) 2017 MongoDB Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_consistency_markers_impl.h"

#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

constexpr StringData ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace;
constexpr StringData ReplicationConsistencyMarkersImpl::kDefaultOplogTruncateAfterPointNamespace;

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
              ReplicationConsistencyMarkersImpl::kDefaultOplogTruncateAfterPointNamespace)) {}

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface,
    NamespaceString minValidNss,
    NamespaceString oplogTruncateAfterPointNss)
    : _storageInterface(storageInterface),
      _minValidNss(minValidNss),
      _oplogTruncateAfterPointNss(oplogTruncateAfterPointNss) {}

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
    LOG(3) << "Initializing minValid document";

    // This initializes the values of the required fields if they are not already set.
    // If one of the fields is already set, the $max will prefer the existing value since it
    // will always be greater than the provided ones.
    TimestampedBSONObj upsert;
    upsert.obj = BSON("$max" << BSON(MinValidDocument::kMinValidTimestampFieldName
                                     << Timestamp()
                                     << MinValidDocument::kMinValidTermFieldName
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
    invariant(doc);  // Initialized at startup so it should never be missing.

    boost::optional<bool> flag = doc->getInitialSyncFlag();
    if (!flag) {
        LOG(3) << "No initial sync flag set, returning initial sync flag value of false.";
        return false;
    }

    LOG(3) << "returning initial sync flag value of " << flag.get();
    return flag.get();
}

void ReplicationConsistencyMarkersImpl::setInitialSyncFlag(OperationContext* opCtx) {
    LOG(3) << "setting initial sync flag";
    TimestampedBSONObj update;
    update.obj = BSON("$set" << kInitialSyncFlag);

    // We do not provide a timestamp when we set the initial sync flag. Initial sync can only
    // occur right when we start up, and thus there cannot be any checkpoints being taken. This
    // write should go into the next checkpoint.
    update.timestamp = Timestamp();

    _updateMinValidDocument(opCtx, update);
    opCtx->recoveryUnit()->waitUntilDurable();
}

void ReplicationConsistencyMarkersImpl::clearInitialSyncFlag(OperationContext* opCtx) {
    LOG(3) << "clearing initial sync flag";

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    OpTime time = replCoord->getMyLastAppliedOpTime();
    TimestampedBSONObj update;
    update.obj = BSON("$unset" << kInitialSyncFlag << "$set"
                               << BSON(MinValidDocument::kMinValidTimestampFieldName
                                       << time.getTimestamp()
                                       << MinValidDocument::kMinValidTermFieldName
                                       << time.getTerm()
                                       << MinValidDocument::kAppliedThroughFieldName
                                       << time));

    // We clear the initial sync flag at the 'lastAppliedOpTime'. This is unnecessary, since there
    // should not be any stable checkpoints being taken that this write could inadvertantly enter.
    // This 'lastAppliedOpTime' will be the first stable timestamp candidate, so it will be in the
    // first stable checkpoint taken after initial sync. This provides more clarity than providing
    // no timestamp.
    update.timestamp = time.getTimestamp();

    _updateMinValidDocument(opCtx, update);

    // Make sure to clear the oplogTrucateAfterPoint in case it is stale. Otherwise, we risk the
    // possibility of deleting oplog entries that we want to keep. It is safe to clear this
    // here since we are consistent at the top of our oplog at this point.
    setOplogTruncateAfterPoint(opCtx, Timestamp());

    if (getGlobalServiceContext()->getStorageEngine()->isDurable()) {
        opCtx->recoveryUnit()->waitUntilDurable();
        replCoord->setMyLastDurableOpTime(time);
    }
}

OpTime ReplicationConsistencyMarkersImpl::getMinValid(OperationContext* opCtx) const {
    auto doc = _getMinValidDocument(opCtx);
    invariant(doc);  // Initialized at startup so it should never be missing.

    auto minValid = OpTime(doc->getMinValidTimestamp(), doc->getMinValidTerm());

    LOG(3) << "returning minvalid: " << minValid.toString() << "(" << minValid.toBSON() << ")";

    return minValid;
}

void ReplicationConsistencyMarkersImpl::setMinValid(OperationContext* opCtx,
                                                    const OpTime& minValid) {
    LOG(3) << "setting minvalid to exactly: " << minValid.toString() << "(" << minValid.toBSON()
           << ")";
    TimestampedBSONObj update;
    update.obj = BSON("$set" << BSON(MinValidDocument::kMinValidTimestampFieldName
                                     << minValid.getTimestamp()
                                     << MinValidDocument::kMinValidTermFieldName
                                     << minValid.getTerm()));

    // This method is only used with storage engines that do not support recover to stable
    // timestamp. As a result, their timestamps do not matter.
    invariant(!opCtx->getServiceContext()->getStorageEngine()->supportsRecoverToStableTimestamp());
    update.timestamp = Timestamp();

    _updateMinValidDocument(opCtx, update);
}

void ReplicationConsistencyMarkersImpl::setMinValidToAtLeast(OperationContext* opCtx,
                                                             const OpTime& minValid) {
    LOG(3) << "setting minvalid to at least: " << minValid.toString() << "(" << minValid.toBSON()
           << ")";

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
                                                          const OpTime& optime) {
    invariant(!optime.isNull());
    LOG(3) << "setting appliedThrough to: " << optime.toString() << "(" << optime.toBSON() << ")";

    // We set the 'appliedThrough' to the provided timestamp. The 'appliedThrough' is only valid
    // in checkpoints that contain all writes through this timestamp since it indicates the top of
    // the oplog.
    TimestampedBSONObj update;
    update.timestamp = optime.getTimestamp();
    update.obj = BSON("$set" << BSON(MinValidDocument::kAppliedThroughFieldName << optime));

    _updateMinValidDocument(opCtx, update);
}

void ReplicationConsistencyMarkersImpl::clearAppliedThrough(OperationContext* opCtx,
                                                            const Timestamp& writeTimestamp) {
    LOG(3) << "clearing appliedThrough at: " << writeTimestamp.toString();

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
        LOG(3) << "No appliedThrough OpTime set, returning empty appliedThrough OpTime.";
        return {};
    }
    LOG(3) << "returning appliedThrough: " << appliedThrough->toString() << "("
           << appliedThrough->toBSON() << ")";

    return appliedThrough.get();
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

void ReplicationConsistencyMarkersImpl::_upsertOplogTruncateAfterPointDocument(
    OperationContext* opCtx, const BSONObj& updateSpec) {
    fassert(40512,
            _storageInterface->upsertById(
                opCtx, _oplogTruncateAfterPointNss, kOplogTruncateAfterPointId["_id"], updateSpec));
}

void ReplicationConsistencyMarkersImpl::setOplogTruncateAfterPoint(OperationContext* opCtx,
                                                                   const Timestamp& timestamp) {
    LOG(3) << "setting oplog truncate after point to: " << timestamp.toBSON();
    _upsertOplogTruncateAfterPointDocument(
        opCtx,
        BSON("$set" << BSON(OplogTruncateAfterPointDocument::kOplogTruncateAfterPointFieldName
                            << timestamp)));
}

Timestamp ReplicationConsistencyMarkersImpl::getOplogTruncateAfterPoint(
    OperationContext* opCtx) const {
    auto doc = _getOplogTruncateAfterPointDocument(opCtx);
    if (!doc) {
        LOG(3) << "Returning empty oplog truncate after point since document did not exist";
        return {};
    }

    Timestamp out = doc->getOplogTruncateAfterPoint();

    LOG(3) << "returning oplog truncate after point: " << out;
    return out;
}

Status ReplicationConsistencyMarkersImpl::createInternalCollections(OperationContext* opCtx) {
    for (auto nss : std::vector<NamespaceString>({_oplogTruncateAfterPointNss, _minValidNss})) {
        auto status = _storageInterface->createCollection(opCtx, nss, CollectionOptions());
        if (!status.isOK() && status.code() != ErrorCodes::NamespaceExists) {
            return {ErrorCodes::CannotCreateCollection,
                    str::stream() << "Failed to create collection. Ns: " << nss.ns() << " Error: "
                                  << status.toString()};
        }
    }

    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
