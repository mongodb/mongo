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


#include "mongo/db/pipeline/change_stream_helpers_legacy.h"

#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/expression.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace change_stream_legacy {

// TODO SERVER-60919 remove 'legacyLookupPreImage' function.
boost::optional<Document> legacyLookupPreImage(boost::intrusive_ptr<ExpressionContext> pExpCtx,
                                               const Document& preImageId) {
    // We need the oplog's UUID for lookup, so obtain the collection info via MongoProcessInterface.
    auto localOplogInfo = pExpCtx->mongoProcessInterface->getCollectionOptions(
        pExpCtx->opCtx, NamespaceString::kRsOplogNamespace);

    // Extract the UUID from the collection information. We should always have a valid uuid here.
    auto oplogUUID = invariantStatusOK(UUID::parse(localOplogInfo["uuid"]));

    // Look up the pre-image oplog entry using the opTime as the query filter.
    const auto opTime = repl::OpTime::parse(preImageId.toBson());
    auto lookedUpDoc =
        pExpCtx->mongoProcessInterface->lookupSingleDocument(pExpCtx,
                                                             NamespaceString::kRsOplogNamespace,
                                                             oplogUUID,
                                                             Document{opTime.asQuery()},
                                                             boost::none);

    // Return boost::none to signify that we failed to find the pre-image.
    if (!lookedUpDoc) {
        return boost::none;
    }

    // If we had an optime to look up, and we found an oplog entry with that timestamp, then we
    // should always have a valid no-op entry containing a valid, non-empty pre-image document.
    auto opLogEntry = uassertStatusOK(repl::OplogEntry::parse(lookedUpDoc->toBson()));
    tassert(5868901,
            "Oplog entry type must be a no-op",
            opLogEntry.getOpType() == repl::OpTypeEnum::kNoop);
    tassert(5868902,
            "Oplog entry must contait a non-empty pre-image document",
            !opLogEntry.getObject().isEmpty());

    return Document{opLogEntry.getObject().getOwned()};
}

// TODO SERVER-66138: This function can be removed after we branch for 7.0.
void populateInternalOperationFilter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     BSONArrayBuilder* orBuilder) {
    std::vector<StringData> opTypes = {"reshardBegin"_sd, "reshardDoneCatchUp"_sd};

    // Noop change events that are only applicable when merging results on mongoS:
    //   - migrateChunkToNewShard: A chunk migrated to a shard that didn't have any chunks.
    if (expCtx->inMongos || expCtx->needsMerge) {
        opTypes.push_back("migrateChunkToNewShard"_sd);
    }

    for (const auto& eventName : opTypes) {
        // Legacy oplog messages used the "o2.type" field to indicate the message type.
        orBuilder->append(BSON("o2.type" << eventName));
    }
}

// TODO SERVER-66138: This function can be removed after we branch for 7.0.
Document convertFromLegacyOplogFormat(const Document& o2Entry, const NamespaceString& nss) {
    auto type = o2Entry["type"];
    if (type.missing()) {
        return o2Entry;
    }

    MutableDocument doc(o2Entry);
    doc.remove("type");

    // This field would be the first field in the new format, but the current change stream code
    // does not depend on the field order.
    doc.addField(type.getString(), Value(nss.toString()));
    return doc.freeze();
}

// TODO SERVER-66138: This function can be removed after we branch for 7.0.
StringData getNewShardDetectedOpName(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // The op name on 6.0 and older versions.
    const StringData kNewShardDetectedOpTypeLegacyName = "kNewShardDetected"_sd;
    return (expCtx->changeStreamTokenVersion == ResumeTokenData::kDefaultTokenVersion)
        ? DocumentSourceChangeStream::kNewShardDetectedOpType
        : kNewShardDetectedOpTypeLegacyName;
}

}  // namespace change_stream_legacy
}  // namespace mongo
