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
constexpr StringData ReplicationConsistencyMarkersImpl::kDefaultCheckpointTimestampNamespace;

namespace {
const BSONObj kInitialSyncFlag(BSON(MinValidDocument::kInitialSyncFlagFieldName << true));
const BSONObj kOplogTruncateAfterPointId(BSON("_id"
                                              << "oplogTruncateAfterPoint"));
const BSONObj kCheckpointTimestampId(BSON("_id"
                                          << "checkpointTimestamp"));
}  // namespace

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface)
    : ReplicationConsistencyMarkersImpl(
          storageInterface,
          NamespaceString(ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace),
          NamespaceString(
              ReplicationConsistencyMarkersImpl::kDefaultOplogTruncateAfterPointNamespace),
          NamespaceString(
              ReplicationConsistencyMarkersImpl::kDefaultCheckpointTimestampNamespace)) {}

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface,
    NamespaceString minValidNss,
    NamespaceString oplogTruncateAfterPointNss,
    NamespaceString checkpointTimestampNss)
    : _storageInterface(storageInterface),
      _minValidNss(minValidNss),
      _oplogTruncateAfterPointNss(oplogTruncateAfterPointNss),
      _checkpointTimestampNss(checkpointTimestampNss) {}

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

void ReplicationConsistencyMarkersImpl::_updateMinValidDocument(OperationContext* opCtx,
                                                                const BSONObj& updateSpec) {
    Status status = _storageInterface->putSingleton(opCtx, _minValidNss, updateSpec);
    invariantOK(status);
}

void ReplicationConsistencyMarkersImpl::initializeMinValidDocument(OperationContext* opCtx) {
    LOG(3) << "Initializing minValid document";

    // This initializes the values of the required fields if they are not already set.
    // If one of the fields is already set, the $max will prefer the existing value since it
    // will always be greater than the provided ones.
    auto spec = BSON("$max" << BSON(MinValidDocument::kMinValidTimestampFieldName
                                    << Timestamp()
                                    << MinValidDocument::kMinValidTermFieldName
                                    << OpTime::kUninitializedTerm));

    Status status = _storageInterface->putSingleton(opCtx, _minValidNss, spec);

    // If the collection doesn't exist, create it and try again.
    if (status == ErrorCodes::NamespaceNotFound) {
        status = _storageInterface->createCollection(opCtx, _minValidNss, CollectionOptions());
        fassertStatusOK(40509, status);

        status = _storageInterface->putSingleton(opCtx, _minValidNss, spec);
    }

    fassertStatusOK(40467, status);
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
    _updateMinValidDocument(opCtx, BSON("$set" << kInitialSyncFlag));
    opCtx->recoveryUnit()->waitUntilDurable();
}

void ReplicationConsistencyMarkersImpl::clearInitialSyncFlag(OperationContext* opCtx) {
    LOG(3) << "clearing initial sync flag";

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    OpTime time = replCoord->getMyLastAppliedOpTime();
    _updateMinValidDocument(opCtx,
                            BSON("$unset" << kInitialSyncFlag << "$set"
                                          << BSON(MinValidDocument::kMinValidTimestampFieldName
                                                  << time.getTimestamp()
                                                  << MinValidDocument::kMinValidTermFieldName
                                                  << time.getTerm()
                                                  << MinValidDocument::kAppliedThroughFieldName
                                                  << time)));

    if (getGlobalServiceContext()->getGlobalStorageEngine()->isDurable()) {
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
    _updateMinValidDocument(opCtx,
                            BSON("$set" << BSON(MinValidDocument::kMinValidTimestampFieldName
                                                << minValid.getTimestamp()
                                                << MinValidDocument::kMinValidTermFieldName
                                                << minValid.getTerm())));
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
    Status status = _storageInterface->updateSingleton(opCtx, _minValidNss, query, updateSpec);
    invariantOK(status);
}

void ReplicationConsistencyMarkersImpl::removeOldOplogDeleteFromPointField(
    OperationContext* opCtx) {
    _updateMinValidDocument(
        opCtx, BSON("$unset" << BSON(MinValidDocument::kOldOplogDeleteFromPointFieldName << 1)));
}

void ReplicationConsistencyMarkersImpl::setAppliedThrough(OperationContext* opCtx,
                                                          const OpTime& optime) {
    LOG(3) << "setting appliedThrough to: " << optime.toString() << "(" << optime.toBSON() << ")";
    if (optime.isNull()) {
        _updateMinValidDocument(
            opCtx, BSON("$unset" << BSON(MinValidDocument::kAppliedThroughFieldName << 1)));
    } else {
        _updateMinValidDocument(
            opCtx, BSON("$set" << BSON(MinValidDocument::kAppliedThroughFieldName << optime)));
    }
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
    auto status = _storageInterface->upsertById(
        opCtx, _oplogTruncateAfterPointNss, kOplogTruncateAfterPointId["_id"], updateSpec);

    // If the collection doesn't exist, creates it and tries again.
    if (status == ErrorCodes::NamespaceNotFound) {
        status = _storageInterface->createCollection(
            opCtx, _oplogTruncateAfterPointNss, CollectionOptions());
        fassertStatusOK(40511, status);

        status = _storageInterface->upsertById(
            opCtx, _oplogTruncateAfterPointNss, kOplogTruncateAfterPointId["_id"], updateSpec);
    }

    fassertStatusOK(40512, status);
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
        if (serverGlobalParams.featureCompatibility.getVersion() !=
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36) {
            LOG(3) << "Falling back on old oplog delete from point because there is no oplog "
                      "truncate after point and we are in FCV 3.4.";
            return _getOldOplogDeleteFromPoint(opCtx);
        }
        LOG(3) << "Returning empty oplog truncate after point since document did not exist";
        return {};
    }

    Timestamp out = doc->getOplogTruncateAfterPoint();

    LOG(3) << "returning oplog truncate after point: " << out;
    return out;
}

Timestamp ReplicationConsistencyMarkersImpl::_getOldOplogDeleteFromPoint(
    OperationContext* opCtx) const {
    auto doc = _getMinValidDocument(opCtx);
    invariant(doc);  // Initialized at startup so it should never be missing.

    auto oplogDeleteFromPoint = doc->getOldOplogDeleteFromPoint();
    if (!oplogDeleteFromPoint) {
        LOG(3) << "No oplogDeleteFromPoint timestamp set, returning empty timestamp.";
        return {};
    }

    LOG(3) << "returning oplog delete from point: " << oplogDeleteFromPoint.get();
    return oplogDeleteFromPoint.get();
}

void ReplicationConsistencyMarkersImpl::_upsertCheckpointTimestampDocument(
    OperationContext* opCtx, const BSONObj& updateSpec) {
    auto status = _storageInterface->upsertById(
        opCtx, _checkpointTimestampNss, kCheckpointTimestampId["_id"], updateSpec);

    // If the collection doesn't exist, creates it and tries again.
    if (status == ErrorCodes::NamespaceNotFound) {
        status = _storageInterface->createCollection(
            opCtx, _checkpointTimestampNss, CollectionOptions());
        fassertStatusOK(40581, status);

        status = _storageInterface->upsertById(
            opCtx, _checkpointTimestampNss, kCheckpointTimestampId["_id"], updateSpec);
    }

    fassertStatusOK(40582, status);
}

void ReplicationConsistencyMarkersImpl::writeCheckpointTimestamp(OperationContext* opCtx,
                                                                 const Timestamp& timestamp) {
    LOG(3) << "setting checkpoint timestamp to: " << timestamp.toBSON();

    auto timestampField = CheckpointTimestampDocument::kCheckpointTimestampFieldName;
    auto spec = BSON("$set" << BSON(timestampField << timestamp));

    // TODO: When SERVER-28602 is completed, utilize RecoveryUnit::setTimestamp so that this
    // write operation itself is committed with a timestamp that is included in the checkpoint.
    _upsertCheckpointTimestampDocument(opCtx, spec);
}

boost::optional<CheckpointTimestampDocument>
ReplicationConsistencyMarkersImpl::_getCheckpointTimestampDocument(OperationContext* opCtx) const {
    auto doc =
        _storageInterface->findById(opCtx, _checkpointTimestampNss, kCheckpointTimestampId["_id"]);

    if (!doc.isOK()) {
        if (doc.getStatus() == ErrorCodes::NoSuchKey ||
            doc.getStatus() == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        } else {
            // Fails if there is an error other than the collection being missing or being empty.
            fassertFailedWithStatus(40583, doc.getStatus());
        }
    }

    auto checkpointTimestampDoc = CheckpointTimestampDocument::parse(
        IDLParserErrorContext("CheckpointTimestampDocument"), doc.getValue());
    return checkpointTimestampDoc;
}

Timestamp ReplicationConsistencyMarkersImpl::getCheckpointTimestamp(OperationContext* opCtx) {
    auto doc = _getCheckpointTimestampDocument(opCtx);
    if (!doc) {
        LOG(3) << "Returning empty checkpoint timestamp since document did not exist";
        return {};
    }

    Timestamp out = doc->getCheckpointTimestamp();

    LOG(3) << "returning checkpoint timestamp: " << out;
    return out;
}


}  // namespace repl
}  // namespace mongo
