
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

namespace {
const BSONObj kInitialSyncFlag(BSON(MinValidDocument::kInitialSyncFlagFieldName << true));
}  // namespace

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface)
    : ReplicationConsistencyMarkersImpl(
          storageInterface,
          NamespaceString(ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace)) {}

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface, NamespaceString minValidNss)
    : _storageInterface(storageInterface), _minValidNss(minValidNss) {}

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

    // If the collection doesn't exist, create it and try again.
    if (status == ErrorCodes::NamespaceNotFound) {
        status = _storageInterface->createCollection(opCtx, _minValidNss, CollectionOptions());
        if (status.isOK()) {
            status = _storageInterface->putSingleton(opCtx, _minValidNss, updateSpec);
        }
    }

    fassertStatusOK(40467, status);
}

void ReplicationConsistencyMarkersImpl::initializeMinValidDocument(OperationContext* opCtx) {
    LOG(3) << "Initializing minValid document";

    // This initializes the values of the required fields if they are not already set.
    // If one of the fields is already set, the $max will prefer the existing value since it
    // will always be greater than the provided ones.
    _updateMinValidDocument(opCtx,
                            BSON("$max" << BSON(MinValidDocument::kMinValidTimestampFieldName
                                                << Timestamp()
                                                << MinValidDocument::kMinValidTermFieldName
                                                << OpTime::kUninitializedTerm
                                                << MinValidDocument::kOplogDeleteFromPointFieldName
                                                << Timestamp())));
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
    _updateMinValidDocument(opCtx,
                            BSON("$max" << BSON(MinValidDocument::kMinValidTimestampFieldName
                                                << minValid.getTimestamp()
                                                << MinValidDocument::kMinValidTermFieldName
                                                << minValid.getTerm())));
}

void ReplicationConsistencyMarkersImpl::setOplogDeleteFromPoint(OperationContext* opCtx,
                                                                const Timestamp& timestamp) {
    LOG(3) << "setting oplog delete from point to: " << timestamp.toStringPretty();
    _updateMinValidDocument(
        opCtx, BSON("$set" << BSON(MinValidDocument::kOplogDeleteFromPointFieldName << timestamp)));
}

Timestamp ReplicationConsistencyMarkersImpl::getOplogDeleteFromPoint(
    OperationContext* opCtx) const {
    auto doc = _getMinValidDocument(opCtx);
    invariant(doc);  // Initialized at startup so it should never be missing.

    auto oplogDeleteFromPoint = doc->getOplogDeleteFromPoint();
    if (!oplogDeleteFromPoint) {
        LOG(3) << "No oplogDeleteFromPoint timestamp set, returning empty timestamp.";
        return Timestamp();
    }

    LOG(3) << "returning oplog delete from point: " << oplogDeleteFromPoint.get();
    return oplogDeleteFromPoint.get();
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

}  // namespace repl
}  // namespace mongo
