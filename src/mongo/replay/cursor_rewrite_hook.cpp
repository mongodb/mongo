// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/cursor_rewrite_hook.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_hook_manager.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {


void CursorRewriteHook::onRequest(ReplayCommand& command) {
    if (command.parseOpType() != "getMore") {
        return;
    }

    auto body = command.fetchMsgRequest().body;
    auto recCursor = body["getMore"].numberLong();
    tassert(10823800, "getMore request has invalid cursor", recCursor);
    auto liveCursor = _cursorMap.find(recCursor);
    if (liveCursor == _cursorMap.end()) {
        return;
    }
    auto newBody = body.addField(BSON("getMore" << liveCursor->second).firstElement());

    Message message;
    message.setData(dbMsg, newBody.objdata(), newBody.objsize());
    OpMsg::removeChecksum(&message);
    OpMsg::appendChecksum(&message);

    LOGV2_DEBUG(10893014, 5, "getMore rewritten", "old"_attr = body, "new"_attr = newBody);
    command.replaceBody(newBody);
}
void CursorRewriteHook::onLiveResponse(const ReplayCommand& recordedResponse,
                                       const BSONObj& liveResponse) {
    const auto& rec = recordedResponse.fetchMsgRequest().body;
    auto recCursor = rec.getField("cursor");
    auto liveCursor = liveResponse.getField("cursor");
    if (recCursor && liveCursor) {
        _cursorMap[recCursor["id"].numberLong()] = liveCursor["id"].numberLong();
    }
}


static const bool _CursorRewriteHookRegistered =
    (ReplayObserverManager::get().registerPerSessionObserver<CursorRewriteHook>(), true);

}  // namespace mongo
