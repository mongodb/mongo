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

#include "mongo/executor/remote_command_request.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace executor {
namespace {

// Used to generate unique identifiers for requests so they can be traced throughout the
// asynchronous networking logs
AtomicWord<unsigned long long> requestIdCounter(0);

}  // namespace

constexpr Milliseconds RemoteCommandRequest::kNoTimeout;

RemoteCommandRequest::RemoteCommandRequest()
    : id(requestIdCounter.addAndFetch(1)), operationKey(UUID::gen()) {}

RemoteCommandRequest::RemoteCommandRequest(RequestId requestId_,
                                           const HostAndPort& target_,
                                           const DatabaseName& dbName_,
                                           const BSONObj& cmdObj_,
                                           const BSONObj& metadataObj_,
                                           OperationContext* opCtx_,
                                           Milliseconds timeoutMillis_,
                                           bool fireAndForget_,
                                           boost::optional<UUID> opKey_)
    : id(requestId_),
      target(target_),
      dbname(dbName_),
      cmdObj(cmdObj_),
      metadata(metadataObj_),
      opCtx(opCtx_),
      timeout(timeoutMillis_),
      fireAndForget(fireAndForget_),
      operationKey(opKey_) {

    // If there is a comment associated with the current operation, append it to the command that we
    // are about to dispatch to the shards.
    if (opCtx && opCtx->getComment() && !cmdObj["comment"]) {
        cmdObj = cmdObj.addField(*opCtx->getComment());
    }

    if (cmdObj.hasField("maxTimeMSOpOnly")) {
        int maxTimeField = cmdObj["maxTimeMSOpOnly"].Number();
        if (auto maxTimeMSOpOnly = Milliseconds(maxTimeField);
            timeout == executor::RemoteCommandRequest::kNoTimeout || maxTimeMSOpOnly < timeout) {
            timeout = maxTimeMSOpOnly;
        }
    }

    if (opCtx && APIParameters::get(opCtx).getParamsPassed()) {
        BSONObjBuilder bob(std::move(cmdObj));
        APIParameters::get(opCtx).appendInfo(&bob);
        cmdObj = bob.obj();
    }

    _updateTimeoutFromOpCtxDeadline(opCtx);
}

RemoteCommandRequest::RemoteCommandRequest(const HostAndPort& target_,
                                           const DatabaseName& dbName_,
                                           const BSONObj& cmdObj_,
                                           const BSONObj& metadataObj_,
                                           OperationContext* opCtx_,
                                           Milliseconds timeoutMillis_,
                                           bool fireAndForget_,
                                           boost::optional<UUID> operationKey_)
    : RemoteCommandRequest(requestIdCounter.addAndFetch(1),
                           target_,
                           dbName_,
                           cmdObj_,
                           metadataObj_,
                           opCtx_,
                           timeoutMillis_,
                           fireAndForget_,
                           operationKey_) {}

RemoteCommandRequest::operator OpMsgRequest() const {
    const auto& tenantId = this->dbname.tenantId();
    const auto vts = tenantId
        ? auth::ValidatedTenancyScopeFactory::create(
              *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{})
        : auth::ValidatedTenancyScope::kNotRequired;

    return OpMsgRequestBuilder::create(vts, this->dbname, std::move(this->cmdObj), this->metadata);
}

void RemoteCommandRequest::_updateTimeoutFromOpCtxDeadline(const OperationContext* opCtx) {
    if (!opCtx || !opCtx->hasDeadline()) {
        return;
    }

    const auto opCtxTimeout = opCtx->getRemainingMaxTimeMillis();
    if (timeout == kNoTimeout || opCtxTimeout <= timeout) {
        timeout = opCtxTimeout;
        timeoutCode = opCtx->getTimeoutError();

        if (MONGO_unlikely(maxTimeNeverTimeOut.shouldFail())) {
            // If a mongod or mongos receives a request with a 'maxTimeMS', but the
            // 'maxTimeNeverTimeOut' failpoint is enabled, that server process should not enforce
            // the deadline locally, but should still pass the remaining deadline on to any other
            // servers it contacts as 'maxTimeMSOpOnly'.
            enforceLocalTimeout = false;
        }
    }
}

std::string RemoteCommandRequest::toString() const {
    str::stream out;
    out << "RemoteCommand " << id << " -- target:";
    out << target.toString();
    out << " db:" << toStringForLogging(dbname);
    out << " fireAndForget:" << fireAndForget;

    if (dateScheduled && timeout != kNoTimeout) {
        out << " expDate:" << (*dateScheduled + timeout).toString();
    }

    if (operationKey) {
        out << " operationKey:" << *operationKey;
    }

    out << " cmd:" << cmdObj.toString();
    return out;
}

bool RemoteCommandRequest::operator==(const RemoteCommandRequest& rhs) const {
    if (this == &rhs) {
        return true;
    }
    return target == rhs.target && dbname == rhs.dbname &&
        SimpleBSONObjComparator::kInstance.evaluate(cmdObj == rhs.cmdObj) &&
        SimpleBSONObjComparator::kInstance.evaluate(metadata == rhs.metadata) &&
        timeout == rhs.timeout;
}

bool RemoteCommandRequest::operator!=(const RemoteCommandRequest& rhs) const {
    return !(*this == rhs);
}
}  // namespace executor
}  // namespace mongo
