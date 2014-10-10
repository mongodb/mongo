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

#include "mongo/bson/optime.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_impl.h"

namespace mongo {
namespace repl {

namespace {
    const char* initialSyncFlagString = "doingInitialSync";
    const BSONObj initialSyncFlag(BSON(initialSyncFlagString << true));
    const char* minvalidNS = "local.replset.minvalid";
} // namespace

    void clearInitialSyncFlag(OperationContext* txn) {
        Lock::DBLock lk(txn->lockState(), "local", MODE_X);
        WriteUnitOfWork wunit(txn);
        Helpers::putSingleton(txn, minvalidNS, BSON("$unset" << initialSyncFlag));
        wunit.commit();
    }

    void setInitialSyncFlag(OperationContext* txn) {
        Lock::DBLock lk(txn->lockState(), "local", MODE_X);
        WriteUnitOfWork wunit(txn);
        Helpers::putSingleton(txn, minvalidNS, BSON("$set" << initialSyncFlag));
        wunit.commit();
    }

    bool getInitialSyncFlag() {
        OperationContextImpl txn; // XXX?
        BSONObj mv;
        if (Helpers::getSingleton(&txn, minvalidNS, mv)) {
            return mv[initialSyncFlagString].trueValue();
        }
        return false;
    }

    void setMinValid(OperationContext* ctx, OpTime ts) {
        Lock::DBLock lk(ctx->lockState(), "local", MODE_X);
        WriteUnitOfWork wunit(ctx);
        Helpers::putSingleton(ctx, minvalidNS, BSON("$set" << BSON("ts" << ts)));
        wunit.commit();
    }

    OpTime getMinValid(OperationContext* txn) {
        BSONObj mv;
        if (Helpers::getSingleton(txn, minvalidNS, mv)) {
            return mv["ts"]._opTime();
        }
        return OpTime();
    }

}
}
