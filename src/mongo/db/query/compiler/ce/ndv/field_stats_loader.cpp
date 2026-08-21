// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ndv/field_stats_loader.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/compiler/ce/ndv/field_stats.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

#include <utility>

namespace mongo::ce {

StatusWith<FieldStatsDoc> loadFieldStats(OperationContext* opCtx,
                                         const DatabaseName& dbName,
                                         const UUID& collectionUuid,
                                         std::vector<std::string> fieldPaths) {
    const NamespaceString nss =
        NamespaceStringUtil::deserialize(dbName, NamespaceString::kStatsFieldStatsCollectionName);
    const std::string docId = makeFieldStatsId(collectionUuid, std::move(fieldPaths));

    // The _id string embeds the schema version, collection UUID and field paths, so an exact
    // _id match is already an exact identity match; the top-level copies of these fields need
    // no re-validation.
    try {
        DBDirectClient client(opCtx);
        const BSONObj doc = client.findOne(nss, BSON("_id" << docId));
        if (doc.isEmpty()) {
            return Status(ErrorCodes::NoSuchKey,
                          str::stream()
                              << "no persisted field statistics with id '" << docId << "'");
        }
        return FieldStatsDoc::parse(doc, IDLParserContext("FieldStatsDoc"));
    } catch (const DBException& ex) {
        return ex.toStatus().withContext(
            str::stream() << "failed to load field statistics with id '" << docId << "'");
    }
}

}  // namespace mongo::ce
