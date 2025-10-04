/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/modules.h"

#include <vector>

namespace MONGO_MOD_PUB mongo {
class BSONObjBuilder;
class OperationContext;

namespace repl {
namespace apply_ops_command_info_details {
bool _parseAreOpsCrudOnly(const BSONObj& applyOpCmd);
}  // namespace apply_ops_command_info_details

class ApplyOps {
public:
    static constexpr StringData kOplogApplicationModeFieldName = "oplogApplicationMode"_sd;

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
}  // namespace MONGO_MOD_PUB mongo
