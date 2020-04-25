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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_lookup_change_pre_image.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/repl/local_oplog_info.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

constexpr StringData DocumentSourceLookupChangePreImage::kStageName;
constexpr StringData DocumentSourceLookupChangePreImage::kFullDocumentBeforeChangeFieldName;

boost::intrusive_ptr<DocumentSourceLookupChangePreImage> DocumentSourceLookupChangePreImage::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    auto mode = spec.getFullDocumentBeforeChange();

    return make_intrusive<DocumentSourceLookupChangePreImage>(expCtx, mode);
}

DocumentSource::GetNextResult DocumentSourceLookupChangePreImage::doGetNext() {
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    // If this is not an update, replace or delete, then just pass along the result.
    const auto kOpTypeField = DocumentSourceChangeStream::kOperationTypeField;
    const auto opType = input.getDocument()[kOpTypeField];
    DocumentSourceChangeStream::checkValueType(opType, kOpTypeField, BSONType::String);
    if (opType.getStringData() != DocumentSourceChangeStream::kUpdateOpType &&
        opType.getStringData() != DocumentSourceChangeStream::kReplaceOpType &&
        opType.getStringData() != DocumentSourceChangeStream::kDeleteOpType) {
        return input;
    }

    // If a pre-image is available, the transform stage will have populated it in the event's
    // 'fullDocumentBeforeChange' field. If this field is missing and the pre-imaging mode is
    // 'required', we throw an exception. Otherwise, we pass along the document unmodified.
    auto preImageOpTimeVal = input.getDocument()[kFullDocumentBeforeChangeFieldName];
    if (preImageOpTimeVal.missing()) {
        uassert(51770,
                str::stream()
                    << "Change stream was configured to require a pre-image for all update, delete "
                       "and replace events, but no pre-image optime was recorded for event: "
                    << input.getDocument().toString(),
                _fullDocumentBeforeChangeMode != FullDocumentBeforeChangeModeEnum::kRequired);
        return input;
    }

    // Look up the pre-image using the optime. This may return boost::none if it was not found.
    auto preImageOpTime = repl::OpTime::parse(preImageOpTimeVal.getDocument().toBson());
    auto preImageDoc = lookupPreImage(input.getDocument(), preImageOpTime);

    // Even if no pre-image was found, we have to replace the 'fullDocumentBeforeChange' field.
    MutableDocument outputDoc(input.releaseDocument());
    outputDoc[kFullDocumentBeforeChangeFieldName] = (preImageDoc ? Value(*preImageDoc) : Value());

    return outputDoc.freeze();
}

boost::optional<Document> DocumentSourceLookupChangePreImage::lookupPreImage(
    const Document& inputDoc, const repl::OpTime& opTime) const {
    // We need the oplog's UUID for lookup, so obtain the collection info via MongoProcessInterface.
    auto localOplogInfo = pExpCtx->mongoProcessInterface->getCollectionOptions(
        pExpCtx->opCtx, NamespaceString::kRsOplogNamespace);

    // Extract the UUID from the collection information. We should always have a valid uuid here.
    auto oplogUUID = invariantStatusOK(UUID::parse(localOplogInfo["uuid"]));

    // Look up the pre-image oplog entry using the opTime as the query filter.
    auto lookedUpDoc =
        pExpCtx->mongoProcessInterface->lookupSingleDocument(pExpCtx,
                                                             NamespaceString::kRsOplogNamespace,
                                                             oplogUUID,
                                                             Document{opTime.asQuery()},
                                                             boost::none);

    // Failing to find an oplog entry implies that the pre-image has rolled off the oplog. This is
    // acceptable if the mode is "kWhenAvailable", but not if the mode is "kRequired".
    if (!lookedUpDoc) {
        uassert(
            ErrorCodes::ChangeStreamHistoryLost,
            str::stream()
                << "Change stream was configured to require a pre-image for all update, delete and "
                   "replace events, but the pre-image was not found in the oplog for event: "
                << inputDoc.toString(),
            _fullDocumentBeforeChangeMode != FullDocumentBeforeChangeModeEnum::kRequired);

        // Return boost::none to signify that we (legally) failed to find the pre-image.
        return boost::none;
    }

    // If we had an optime to look up, and we found an oplog entry with that timestamp, then we
    // should always have a valid no-op entry containing a valid, non-empty pre-image document.
    auto opLogEntry = invariantStatusOK(repl::OplogEntry::parse(lookedUpDoc->toBson()));
    invariant(opLogEntry.getOpType() == repl::OpTypeEnum::kNoop);
    invariant(!opLogEntry.getObject().isEmpty());

    return Document{opLogEntry.getObject().getOwned()};
}

}  // namespace mongo
