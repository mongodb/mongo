// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
    std::vector<boost::optional<MultikeyPaths>> multikey;
    boost::optional<Status> cause;
    repl::OpTime opTime;
    boost::optional<std::string> indexBuildIdent;
};

}  // namespace mongo
