
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
constexpr StringData ReplicationConsistencyMarkersImpl::kInitialSyncFlagFieldName;
constexpr StringData ReplicationConsistencyMarkersImpl::kBeginFieldName;
constexpr StringData ReplicationConsistencyMarkersImpl::kOplogDeleteFromPointFieldName;

namespace {
const BSONObj kInitialSyncFlag(BSON(ReplicationConsistencyMarkersImpl::kInitialSyncFlagFieldName
                                    << true));
}  // namespace

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface)
    : ReplicationConsistencyMarkersImpl(
          storageInterface,
          NamespaceString(ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace)) {}

ReplicationConsistencyMarkersImpl::ReplicationConsistencyMarkersImpl(
    StorageInterface* storageInterface, NamespaceString minValidNss)
    : _storageInterface(storageInterface), _minValidNss(minValidNss) {}

BSONObj ReplicationConsistencyMarkersImpl::_getMinValidDocument(OperationContext* opCtx) const {
    auto result = _storageInterface->findSingleton(opCtx, _minValidNss);
    if (!result.isOK()) {
        if (result.getStatus() == ErrorCodes::NamespaceNotFound ||
            result.getStatus() == ErrorCodes::CollectionIsEmpty) {
            return BSONObj();
        }
        // Fail if there is an error other than the collection being missing or being empty.
        fassertFailedWithStatus(40466, result.getStatus());
    }
    return result.getValue();
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

bool ReplicationConsistencyMarkersImpl::getInitialSyncFlag(OperationContext* opCtx) const {
    const BSONObj doc = _getMinValidDocument(opCtx);
    const auto flag = doc[kInitialSyncFlagFieldName].trueValue();
    LOG(3) << "returning initial sync flag value of " << flag;
    return flag;
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
    _updateMinValidDocument(
        opCtx,
        BSON("$unset" << kInitialSyncFlag << "$set"
                      << BSON("ts" << time.getTimestamp() << "t" << time.getTerm()
                                   << kBeginFieldName
                                   << time)));

    if (getGlobalServiceContext()->getGlobalStorageEngine()->isDurable()) {
        opCtx->recoveryUnit()->waitUntilDurable();
        replCoord->setMyLastDurableOpTime(time);
    }
}

OpTime ReplicationConsistencyMarkersImpl::getMinValid(OperationContext* opCtx) const {
    const BSONObj doc = _getMinValidDocument(opCtx);
    const auto opTimeStatus = OpTime::parseFromOplogEntry(doc);
    // If any of the keys (fields) are missing from the minvalid document, we return
    // a null OpTime.
    if (opTimeStatus == ErrorCodes::NoSuchKey) {
        return {};
    }

    if (!opTimeStatus.isOK()) {
        severe() << "Error parsing minvalid entry: " << redact(doc)
                 << ", with status:" << opTimeStatus.getStatus();
        fassertFailedNoTrace(40052);
    }

    OpTime minValid = opTimeStatus.getValue();
    LOG(3) << "returning minvalid: " << minValid.toString() << "(" << minValid.toBSON() << ")";

    return minValid;
}

void ReplicationConsistencyMarkersImpl::setMinValid(OperationContext* opCtx,
                                                    const OpTime& minValid) {
    LOG(3) << "setting minvalid to exactly: " << minValid.toString() << "(" << minValid.toBSON()
           << ")";
    _updateMinValidDocument(
        opCtx, BSON("$set" << BSON("ts" << minValid.getTimestamp() << "t" << minValid.getTerm())));
}

void ReplicationConsistencyMarkersImpl::setMinValidToAtLeast(OperationContext* opCtx,
                                                             const OpTime& minValid) {
    LOG(3) << "setting minvalid to at least: " << minValid.toString() << "(" << minValid.toBSON()
           << ")";
    _updateMinValidDocument(
        opCtx, BSON("$max" << BSON("ts" << minValid.getTimestamp() << "t" << minValid.getTerm())));
}

void ReplicationConsistencyMarkersImpl::setOplogDeleteFromPoint(OperationContext* opCtx,
                                                                const Timestamp& timestamp) {
    LOG(3) << "setting oplog delete from point to: " << timestamp.toStringPretty();
    _updateMinValidDocument(opCtx,
                            BSON("$set" << BSON(kOplogDeleteFromPointFieldName << timestamp)));
}

Timestamp ReplicationConsistencyMarkersImpl::getOplogDeleteFromPoint(
    OperationContext* opCtx) const {
    const BSONObj doc = _getMinValidDocument(opCtx);
    Timestamp out = {};
    if (auto field = doc[kOplogDeleteFromPointFieldName]) {
        out = field.timestamp();
    }

    LOG(3) << "returning oplog delete from point: " << out;
    return out;
}

void ReplicationConsistencyMarkersImpl::setAppliedThrough(OperationContext* opCtx,
                                                          const OpTime& optime) {
    LOG(3) << "setting appliedThrough to: " << optime.toString() << "(" << optime.toBSON() << ")";
    if (optime.isNull()) {
        _updateMinValidDocument(opCtx, BSON("$unset" << BSON(kBeginFieldName << 1)));
    } else {
        _updateMinValidDocument(opCtx, BSON("$set" << BSON(kBeginFieldName << optime)));
    }
}

OpTime ReplicationConsistencyMarkersImpl::getAppliedThrough(OperationContext* opCtx) const {
    const BSONObj doc = _getMinValidDocument(opCtx);
    const auto opTimeStatus = OpTime::parseFromOplogEntry(doc.getObjectField(kBeginFieldName));
    if (!opTimeStatus.isOK()) {
        // Return null OpTime on any parse failure, including if "begin" is missing.
        return {};
    }

    OpTime appliedThrough = opTimeStatus.getValue();
    LOG(3) << "returning appliedThrough: " << appliedThrough.toString() << "("
           << appliedThrough.toBSON() << ")";

    return appliedThrough;
}

}  // namespace repl
}  // namespace mongo
