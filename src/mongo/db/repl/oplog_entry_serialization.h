// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/session/logical_session_id.h"  // for StmtId.
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace repl {
void zeroOneManyStmtIdAppend(const std::vector<StmtId>& stmtIds,
                             std::string_view fieldName,
                             BSONObjBuilder* bob);

std::vector<StmtId> parseZeroOneManyStmtId(const BSONElement& element);

}  // namespace repl
}  // namespace mongo
