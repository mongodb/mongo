// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/curop_failpoint_helpers.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
// Fields in a failpoint's data which scope the failpoint to a subset of the operations reaching a
// 'waitWhileFailPointEnabled' callsite.
constexpr auto kCommentFieldName = "comment"sv;
constexpr auto kNssFieldName = "nss"sv;
}  // namespace

std::string CurOpFailpointHelpers::updateCurOpFailPointMsg(OperationContext* opCtx,
                                                           const std::string& newMsg) {
    std::lock_guard<Client> lk(*opCtx->getClient());
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
            // Hang only the queries matching the comment field if one is specified.
            if (auto targeted = data[kCommentFieldName]; !targeted.eoo()) {
                auto comment = opCtx->getComment();
                return comment && comment->woCompare(targeted) == 0;
            }
            // Hang only the queries matching the nss field if a non-empty one is specified. An
            // empty 'nss' means "do not scope by namespace", which some tests rely on when they
            // build the failpoint data unconditionally.
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, kNssFieldName);
            if (!fpNss.isEmpty()) {
                // The callsite must pass the namespace of the operation being evaluated, otherwise
                // the scoping would be silently ignored and the failpoint would match every
                // operation reaching this callsite.
                uassert(13238000,
                        str::stream() << "failpoint data specifies a non-empty '" << kNssFieldName
                                      << "' but the callsite of waitWhileFailPointEnabled does not "
                                         "pass a namespace",
                        !nss.isEmpty());
                return fpNss == nss;
            }
            return true;
        });
}
}  // namespace mongo
