/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/repl/minvalid.h"

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {
const char kInitialSyncFlagFieldName[] = "doingInitialSync";
const BSONObj kInitialSyncFlag(BSON(kInitialSyncFlagFieldName << true));
NamespaceString minValidNss("local.replset.minvalid");
const char kBeginFieldName[] = "begin";
const char kOplogDeleteFromPointFieldName[] = "oplogDeleteFromPoint";

BSONObj getMinValidDocument(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IS);
        Lock::DBLock dblk(txn->lockState(), minValidNss.db(), MODE_IS);
        Lock::CollectionLock lk(txn->lockState(), minValidNss.ns(), MODE_IS);
        BSONObj doc;
        bool found = Helpers::getSingleton(txn, minValidNss.ns().c_str(), doc);
        invariant(found || doc.isEmpty());
        return doc;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "getMinValidDocument", minValidNss.ns());

    MONGO_UNREACHABLE;
}

void updateMinValidDocument(OperationContext* txn, const BSONObj& updateSpec) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        // For now this needs to be MODE_X because it sometimes creates the collection.
        Lock::DBLock dblk(txn->lockState(), minValidNss.db(), MODE_X);
        Helpers::putSingleton(txn, minValidNss.ns().c_str(), updateSpec);
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "updateMinValidDocument", minValidNss.ns());
}
}  // namespace

// Writes
void clearInitialSyncFlag(OperationContext* txn) {
    auto replCoord = repl::ReplicationCoordinator::get(txn);
    OpTime time = replCoord->getMyLastAppliedOpTime();
    updateMinValidDocument(txn,
                           BSON("$unset"
                                << kInitialSyncFlag << "$set"
                                << BSON("ts" << time.getTimestamp() << "t" << time.getTerm()
                                             << kBeginFieldName << time.toBSON())));
    txn->recoveryUnit()->waitUntilDurable();
    replCoord->setMyLastDurableOpTime(time);
    LOG(3) << "clearing initial sync flag";
}

void setInitialSyncFlag(OperationContext* txn) {
    updateMinValidDocument(txn, BSON("$set" << kInitialSyncFlag));
    txn->recoveryUnit()->waitUntilDurable();
    LOG(3) << "setting initial sync flag";
}

bool getInitialSyncFlag() {
    OperationContextImpl txn;
    return getInitialSyncFlag(&txn);
}
bool getInitialSyncFlag(OperationContext* txn) {
    const BSONObj doc = getMinValidDocument(txn);
    const auto flag = doc[kInitialSyncFlagFieldName].trueValue();
    LOG(3) << "returning initial sync flag value of " << flag;
    return flag;
}

OpTime getMinValid(OperationContext* txn) {
    const BSONObj doc = getMinValidDocument(txn);
    const auto opTimeStatus = OpTime::parseFromOplogEntry(doc);
    // If any of the keys (fields) are missing from the minvalid document, we return
    // a null OpTime.
    if (opTimeStatus == ErrorCodes::NoSuchKey) {
        return {};
    }

    if (!opTimeStatus.isOK()) {
        severe() << "Error parsing minvalid entry: " << doc
                 << ", with status:" << opTimeStatus.getStatus();
        fassertFailedNoTrace(40052);
    }

    OpTime minValid = opTimeStatus.getValue();
    LOG(3) << "returning minvalid: " << minValid.toString() << "(" << minValid.toBSON() << ")";

    return minValid;
}
void setMinValid(OperationContext* txn, const OpTime& minValid) {
    LOG(3) << "setting minvalid to exactly: " << minValid.toString() << "(" << minValid.toBSON()
           << ")";
    updateMinValidDocument(
        txn, BSON("$set" << BSON("ts" << minValid.getTimestamp() << "t" << minValid.getTerm())));
}

void setMinValidToAtLeast(OperationContext* txn, const OpTime& minValid) {
    LOG(3) << "setting minvalid to at least: " << minValid.toString() << "(" << minValid.toBSON()
           << ")";
    updateMinValidDocument(
        txn, BSON("$max" << BSON("ts" << minValid.getTimestamp() << "t" << minValid.getTerm())));
}

void setOplogDeleteFromPoint(OperationContext* txn, const Timestamp& timestamp) {
    LOG(3) << "setting oplog delete from point to: " << timestamp.toStringPretty();
    updateMinValidDocument(txn, BSON("$set" << BSON(kOplogDeleteFromPointFieldName << timestamp)));
}

Timestamp getOplogDeleteFromPoint(OperationContext* txn) {
    const BSONObj doc = getMinValidDocument(txn);
    Timestamp out = {};
    if (auto field = doc[kOplogDeleteFromPointFieldName]) {
        out = field.timestamp();
    }

    LOG(3) << "returning oplog delete from point: " << out;
    return out;
}

void setAppliedThrough(OperationContext* txn, const OpTime& optime) {
    LOG(3) << "setting appliedThrough to: " << optime.toString() << "(" << optime.toBSON() << ")";
    if (optime.isNull()) {
        updateMinValidDocument(txn, BSON("$unset" << BSON(kBeginFieldName << 1)));
    } else {
        updateMinValidDocument(txn, BSON("$set" << BSON(kBeginFieldName << optime.toBSON())));
    }
}

OpTime getAppliedThrough(OperationContext* txn) {
    const BSONObj doc = getMinValidDocument(txn);
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
