/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/repl/storage_interface_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

const char StorageInterfaceImpl::kDefaultMinValidNamespace[] = "local.replset.minvalid";
const char StorageInterfaceImpl::kInitialSyncFlagFieldName[] = "doingInitialSync";
const char StorageInterfaceImpl::kBeginFieldName[] = "begin";

namespace {

const BSONObj kInitialSyncFlag(BSON(StorageInterfaceImpl::kInitialSyncFlagFieldName << true));

}  // namespace

StorageInterfaceImpl::StorageInterfaceImpl()
    : StorageInterfaceImpl(NamespaceString(StorageInterfaceImpl::kDefaultMinValidNamespace)) {}

StorageInterfaceImpl::StorageInterfaceImpl(const NamespaceString& minValidNss)
    : _minValidNss(minValidNss) {}

NamespaceString StorageInterfaceImpl::getMinValidNss() const {
    return _minValidNss;
}

bool StorageInterfaceImpl::getInitialSyncFlag(OperationContext* txn) const {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IS);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_IS);
        Lock::CollectionLock lk(txn->lockState(), _minValidNss.ns(), MODE_IS);
        BSONObj mv;
        bool found = Helpers::getSingleton(txn, _minValidNss.ns().c_str(), mv);

        if (found) {
            const auto flag = mv[kInitialSyncFlagFieldName].trueValue();
            LOG(3) << "return initial flag value of " << flag;
            return flag;
        }
        LOG(3) << "return initial flag value of false";
        return false;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::getInitialSyncFlag", _minValidNss.ns());

    MONGO_UNREACHABLE;
}

void StorageInterfaceImpl::setInitialSyncFlag(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_X);
        Helpers::putSingleton(txn, _minValidNss.ns().c_str(), BSON("$set" << kInitialSyncFlag));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::setInitialSyncFlag", _minValidNss.ns());

    txn->recoveryUnit()->waitUntilDurable();
    LOG(3) << "setting initial sync flag";
}

void StorageInterfaceImpl::clearInitialSyncFlag(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        // TODO: Investigate correctness of taking MODE_IX for DB/Collection locks
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_X);
        Helpers::putSingleton(txn, _minValidNss.ns().c_str(), BSON("$unset" << kInitialSyncFlag));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::clearInitialSyncFlag", _minValidNss.ns());

    auto replCoord = repl::ReplicationCoordinator::get(txn);
    if (getGlobalServiceContext()->getGlobalStorageEngine()->isDurable()) {
        OpTime time = replCoord->getMyLastAppliedOpTime();
        txn->recoveryUnit()->waitUntilDurable();
        replCoord->setMyLastDurableOpTime(time);
    }
    LOG(3) << "clearing initial sync flag";
}

BatchBoundaries StorageInterfaceImpl::getMinValid(OperationContext* txn) const {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IS);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_IS);
        Lock::CollectionLock lk(txn->lockState(), _minValidNss.ns(), MODE_IS);
        BSONObj mv;
        bool found = Helpers::getSingleton(txn, _minValidNss.ns().c_str(), mv);
        if (found) {
            auto status = OpTime::parseFromOplogEntry(mv.getObjectField(kBeginFieldName));
            OpTime start(status.isOK() ? status.getValue() : OpTime{});
            OpTime end(fassertStatusOK(40052, OpTime::parseFromOplogEntry(mv)));
            LOG(3) << "returning minvalid: " << start.toString() << "(" << start.toBSON() << ") -> "
                   << end.toString() << "(" << end.toBSON() << ")";

            return BatchBoundaries(start, end);
        }
        LOG(3) << "returning empty minvalid";
        return BatchBoundaries{OpTime{}, OpTime{}};
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::getMinValid", _minValidNss.ns());
}

void StorageInterfaceImpl::setMinValid(OperationContext* txn,
                                       const OpTime& endOpTime,
                                       const DurableRequirement durReq) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_X);
        Helpers::putSingleton(
            txn,
            _minValidNss.ns().c_str(),
            BSON("$set" << BSON("ts" << endOpTime.getTimestamp() << "t" << endOpTime.getTerm())
                        << "$unset"
                        << BSON(kBeginFieldName << 1)));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::setMinValid", _minValidNss.ns());

    if (durReq == DurableRequirement::Strong) {
        txn->recoveryUnit()->waitUntilDurable();
    }
    LOG(3) << "setting minvalid: " << endOpTime.toString() << "(" << endOpTime.toBSON() << ")";
}

void StorageInterfaceImpl::setMinValid(OperationContext* txn, const BatchBoundaries& boundaries) {
    const OpTime& start(boundaries.start);
    const OpTime& end(boundaries.end);
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), _minValidNss.db(), MODE_X);
        Helpers::putSingleton(txn,
                              _minValidNss.ns().c_str(),
                              BSON("$set" << BSON("ts" << end.getTimestamp() << "t" << end.getTerm()
                                                       << kBeginFieldName
                                                       << start.toBSON())));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "StorageInterfaceImpl::setMinValid", _minValidNss.ns());
    // NOTE: No need to ensure durability here since starting a batch isn't a problem unless
    // writes happen after, in which case this marker (minvalid) will be written already.
    LOG(3) << "setting minvalid: " << boundaries.start.toString() << "("
           << boundaries.start.toBSON() << ") -> " << boundaries.end.toString() << "("
           << boundaries.end.toBSON() << ")";
}

}  // namespace repl
}  // namespace mongo
