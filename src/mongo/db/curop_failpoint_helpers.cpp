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

#include "mongo/db/curop_failpoint_helpers.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/time_support.h"

#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {

std::string CurOpFailpointHelpers::updateCurOpFailPointMsg(OperationContext* opCtx,
                                                           const std::string& newMsg) {
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    auto oldMsg = CurOp::get(opCtx)->getFailPointMessage();
    CurOp::get(opCtx)->setFailPointMessage(lk, newMsg.c_str());
    return oldMsg;
}

void CurOpFailpointHelpers::waitWhileFailPointEnabled(FailPoint* failPoint,
                                                      OperationContext* opCtx,
                                                      const std::string& failpointMsg,
                                                      const std::function<void()>& whileWaiting,
                                                      const NamespaceString& nss) {
    invariant(failPoint);
    failPoint->executeIf(
        [&](const BSONObj& data) {
            auto origCurOpFailpointMsg = updateCurOpFailPointMsg(opCtx, failpointMsg);

            const bool shouldCheckForInterrupt = data["shouldCheckForInterrupt"].booleanSafe();
            const bool shouldContinueOnInterrupt = data["shouldContinueOnInterrupt"].booleanSafe();
            const Milliseconds sleepForMs = data.hasField("sleepFor")
                ? Milliseconds(data["sleepFor"].numberInt())
                : Milliseconds(10);
            while (MONGO_unlikely(failPoint->shouldFail())) {
                sleepFor(sleepForMs);
                if (whileWaiting) {
                    whileWaiting();
                }

                // Check for interrupt so that an operation can be killed while waiting for the
                // failpoint to be disabled, if the failpoint is configured to be interruptible.
                //
                // For shouldContinueOnInterrupt, an interrupt merely allows the code to continue
                // past the failpoint; it is up to the code under test to actually check for
                // interrupt.
                if (shouldContinueOnInterrupt) {
                    if (!opCtx->checkForInterruptNoAssert().isOK())
                        break;
                } else if (shouldCheckForInterrupt) {
                    opCtx->checkForInterrupt();
                }
                if (data.hasField("sleepFor")) {
                    break;
                }
            }
            updateCurOpFailPointMsg(opCtx, origCurOpFailpointMsg);
        },
        [&](const BSONObj& data) {
            if (data.hasField("comment") && opCtx->getComment()) {
                return opCtx->getComment()->String() == data.getStringField("comment");
            }
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "nss"_sd);
            return nss.isEmpty() || fpNss.isEmpty() || fpNss == nss;
        });
}
}  // namespace mongo
