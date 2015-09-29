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
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {
const char initialSyncFlagString[] = "doingInitialSync";
const BSONObj initialSyncFlag(BSON(initialSyncFlagString << true));
const char minvalidNS[] = "local.replset.minvalid";
const char beginFieldName[] = "begin";
}  // namespace

// Writes
void clearInitialSyncFlag(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        // TODO: Investigate correctness of taking MODE_IX for DB/Collection locks
        Lock::DBLock dblk(txn->lockState(), "local", MODE_X);
        Helpers::putSingleton(txn, minvalidNS, BSON("$unset" << initialSyncFlag));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "clearInitialSyncFlags", minvalidNS);

    txn->recoveryUnit()->waitUntilDurable();
    LOG(3) << "clearing initial sync flag";
}

void setInitialSyncFlag(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), "local", MODE_X);
        Helpers::putSingleton(txn, minvalidNS, BSON("$set" << initialSyncFlag));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "setInitialSyncFlags", minvalidNS);

    txn->recoveryUnit()->waitUntilDurable();
    LOG(3) << "setting initial sync flag";
}

void setMinValid(OperationContext* txn, const OpTime& endOpTime, const DurableRequirement durReq) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), "local", MODE_X);
        Helpers::putSingleton(
            txn,
            minvalidNS,
            BSON("$set" << BSON("ts" << endOpTime.getTimestamp() << "t" << endOpTime.getTerm())
                        << "$unset" << BSON(beginFieldName << 1)));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "setMinValid", minvalidNS);

    if (durReq == DurableRequirement::Strong) {
        txn->recoveryUnit()->waitUntilDurable();
    }
    LOG(3) << "setting minvalid: " << endOpTime.toString() << "(" << endOpTime.toBSON() << ")";
}

void setMinValid(OperationContext* txn, const BatchBoundaries& boundaries) {
    const OpTime& start(boundaries.start);
    const OpTime& end(boundaries.end);
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dblk(txn->lockState(), "local", MODE_X);
        Helpers::putSingleton(txn,
                              minvalidNS,
                              BSON("$set" << BSON("ts" << end.getTimestamp() << "t" << end.getTerm()
                                                       << beginFieldName << start.toBSON())));
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "setMinValid", minvalidNS);
    // NOTE: No need to ensure durability here since starting a batch isn't a problem unless
    // writes happen after, in which case this marker (minvalid) will be written already.
    LOG(3) << "setting minvalid: " << boundaries.start.toString() << "("
           << boundaries.start.toBSON() << ") -> " << boundaries.end.toString() << "("
           << boundaries.end.toBSON() << ")";
}

// Reads
bool getInitialSyncFlag() {
    OperationContextImpl txn;
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(&txn, MODE_IS);
        Lock::DBLock dblk(txn.lockState(), "local", MODE_IS);
        Lock::CollectionLock lk(txn.lockState(), minvalidNS, MODE_IS);
        BSONObj mv;
        bool found = Helpers::getSingleton(&txn, minvalidNS, mv);

        if (found) {
            const auto flag = mv[initialSyncFlagString].trueValue();
            LOG(3) << "return initial flag value of " << flag;
            return flag;
        }
        LOG(3) << "return initial flag value of false";
        return false;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(&txn, "getInitialSyncFlags", minvalidNS);

    MONGO_UNREACHABLE;
}

BatchBoundaries getMinValid(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IS);
        Lock::DBLock dblk(txn->lockState(), "local", MODE_IS);
        Lock::CollectionLock lk(txn->lockState(), minvalidNS, MODE_IS);
        BSONObj mv;
        bool found = Helpers::getSingleton(txn, minvalidNS, mv);
        if (found) {
            auto status = OpTime::parseFromOplogEntry(mv.getObjectField(beginFieldName));
            OpTime start(status.isOK() ? status.getValue() : OpTime{});
            OpTime end(fassertStatusOK(28771, OpTime::parseFromOplogEntry(mv)));
            LOG(3) << "returning minvalid: " << start.toString() << "(" << start.toBSON() << ") -> "
                   << end.toString() << "(" << end.toBSON() << ")";

            return BatchBoundaries(start, end);
        }
        LOG(3) << "returning empty minvalid";
        return BatchBoundaries{OpTime{}, OpTime{}};
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "getMinValid", minvalidNS);
}
}
}
