/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class IndexBuildOplogEntry {
public:
    /**
     * Parses an oplog entry for "startIndexBuild", "commitIndexBuild", or "abortIndexBuild".
     */
    static StatusWith<IndexBuildOplogEntry> parse(OperationContext* opCtx,
                                                  const repl::OplogEntry& entry,
                                                  bool parseO2 = true);

    UUID collUUID;
    repl::OplogEntry::CommandType commandType;
    std::string commandName;
    IndexBuildMethodEnum indexBuildMethod{IndexBuildMethodEnum::kHybrid};
    UUID buildUUID;
    std::vector<IndexBuildInfo> indexes;
    boost::optional<Status> cause;
    repl::OpTime opTime;
};

}  // namespace mongo
