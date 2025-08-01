/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

class CurrentOpStage final : public Stage {
public:
    using ConnMode = MongoProcessInterface::CurrentOpConnectionsMode;
    using SessionMode = MongoProcessInterface::CurrentOpSessionsMode;
    using UserMode = MongoProcessInterface::CurrentOpUserMode;
    using TruncationMode = MongoProcessInterface::CurrentOpTruncateMode;
    using CursorMode = MongoProcessInterface::CurrentOpCursorMode;

    static constexpr StringData kStageName = "$currentOp"_sd;

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
