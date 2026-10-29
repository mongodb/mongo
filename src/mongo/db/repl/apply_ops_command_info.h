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

/**
 * Returns the number of operations in an applyOps, given its array or the oplog entry carrying it.
 */
inline std::size_t numOperationsInApplyOps(const Value& applyOpsArray) {
    return applyOpsArray.missing() ? 0U : applyOpsArray.getArrayLength();
}

inline std::size_t numOperationsInApplyOps(const OplogEntry& applyOpsEntry) {
    return static_cast<std::size_t>(
        applyOpsEntry.getObject()[ApplyOpsCommandInfoBase::kOperationsFieldName].Obj().nFields());
}

/**
 * Returns the total number of operations in the applyOps chain terminated by 'entry': its 'count'
 * field when present (a multi-entry batch), otherwise this entry's own operation count (a
 * single-entry batch).
 */
inline std::size_t applyOpsChainOperationTotal(const OplogEntry& entry) {
    const auto count = entry.getObject()[ApplyOpsCommandInfoBase::kCountFieldName];
    return count.eoo() ? numOperationsInApplyOps(entry)
                       : static_cast<std::size_t>(count.numberLong());
}

/**
 * Saturating 'totalOps - opsAlreadyCollected', for bounding an applyOps chain walk by the
 * terminal's 'count'. See walkApplyOpsChain().
 */
inline std::size_t remainingApplyOpsChainOps(std::size_t totalOps,
                                             std::size_t opsAlreadyCollected) {
    return totalOps > opsAlreadyCollected ? totalOps - opsAlreadyCollected : 0;
}

}  // namespace repl
}  // namespace mongo
