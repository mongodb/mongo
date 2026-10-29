// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_hook_manager.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

/**
 * Hook informed of every request and response for a single session
 * during a replay of a traffic recording, used to re-write
 * cursor responses.
 */
class CursorRewriteHook : public ReplayObserver {
public:
    void onRequest(ReplayCommand& command) override;
    void onLiveResponse(const ReplayCommand& recordedResponse,
                        const BSONObj& liveResponse) override;

private:
    stdx::unordered_map<int64_t, int64_t> _cursorMap;
};

}  // namespace mongo
