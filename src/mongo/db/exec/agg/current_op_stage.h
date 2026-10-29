// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {
using namespace std::literals::string_view_literals;

class CurrentOpStage final : public Stage {
public:
    using ConnMode = MongoProcessInterface::CurrentOpConnectionsMode;
    using SessionMode = MongoProcessInterface::CurrentOpSessionsMode;
    using UserMode = MongoProcessInterface::CurrentOpUserMode;
    using TruncationMode = MongoProcessInterface::CurrentOpTruncateMode;
    using CursorMode = MongoProcessInterface::CurrentOpCursorMode;

    static constexpr std::string_view kStageName = "$currentOp"sv;

    static constexpr ConnMode kDefaultConnMode = ConnMode::kExcludeIdle;
    static constexpr SessionMode kDefaultSessionMode = SessionMode::kIncludeIdle;
    static constexpr UserMode kDefaultUserMode = UserMode::kExcludeOthers;
    static constexpr TruncationMode kDefaultTruncationMode = TruncationMode::kNoTruncation;
    static constexpr CursorMode kDefaultCursorMode = CursorMode::kExcludeCursors;

    CurrentOpStage(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                   const boost::optional<ConnMode>& includeIdleConnections,
                   const boost::optional<SessionMode>& includeIdleSessions,
                   const boost::optional<UserMode>& includeOpsFromAllUsers,
                   const boost::optional<TruncationMode>& truncateOps,
                   const boost::optional<CursorMode>& idleCursors);
    ~CurrentOpStage() override {}

private:
    GetNextResult doGetNext() final;

    // initialized from DocumentSourceCurrentOp
    boost::optional<ConnMode> _includeIdleConnections;
    boost::optional<SessionMode> _includeIdleSessions;
    boost::optional<UserMode> _includeOpsFromAllUsers;
    boost::optional<TruncationMode> _truncateOps;
    boost::optional<CursorMode> _idleCursors;

    std::string _shardName;

    std::vector<BSONObj> _ops;
    std::vector<BSONObj>::iterator _opsIter;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
