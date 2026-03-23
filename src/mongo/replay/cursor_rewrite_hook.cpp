/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
