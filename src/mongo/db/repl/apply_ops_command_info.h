// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {
class BSONObjBuilder;
class OperationContext;

namespace repl {
using namespace std::literals::string_view_literals;
namespace apply_ops_command_info_details {
bool _parseAreOpsCrudOnly(const BSONObj& applyOpCmd);
}  // namespace apply_ops_command_info_details

class ApplyOps {
public:
    static constexpr std::string_view kOplogApplicationModeFieldName = "oplogApplicationMode"sv;

    /**
     * Extracts CRUD operations from an applyOps oplog entry. Throws UserException on error.
     */
    static std::vector<OplogEntry> extractOperations(const OplogEntry& applyOpsOplogEntry);

    /**
     * This variant allows optimization for extracting multiple applyOps operations.  The entry for
     * the non-DurableReplOperation fields of the extracted operation must be specified as
     * 'topLevelDoc', and need not be any of the applyOps operations. The 'topLevelDoc' entry's
     * 'ts' field will be used as the 'ts' field for each operation.
     */
    static void extractOperationsTo(const OplogEntry& applyOpsOplogEntry,
                                    const BSONObj& topLevelDoc,
                                    std::vector<OplogEntry>* operations);
};

/**
 * Holds information about an applyOps command object.
 */
class ApplyOpsCommandInfo : public ApplyOpsCommandInfoBase {
public:
    /**
     * Parses the object in the 'o' field of an applyOps command.
     * May throw UserException.
     */
    static ApplyOpsCommandInfo parse(const BSONObj& applyOpCmd);

    /**
     * Returns true if all operations described by this applyOps command are CRUD only.
     */
    bool areOpsCrudOnly() const;

private:
    explicit ApplyOpsCommandInfo(const BSONObj& applyOpCmd);

    const bool _areOpsCrudOnly;
};

}  // namespace repl
}  // namespace mongo
