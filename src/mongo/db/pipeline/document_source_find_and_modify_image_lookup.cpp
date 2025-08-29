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


// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalFindAndModifyImageLookup,
                                  LiteParsedDocumentSourceInternal::parse,
                                  DocumentSourceFindAndModifyImageLookup::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalFindAndModifyImageLookup,
                            DocumentSourceFindAndModifyImageLookup::id)

using OplogEntry = repl::OplogEntryBase;

boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup>
DocumentSourceFindAndModifyImageLookup::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool includeCommitTimestamp) {
    return new DocumentSourceFindAndModifyImageLookup(expCtx, includeCommitTimestamp);
}

boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup>
DocumentSourceFindAndModifyImageLookup::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5806003,
            str::stream() << "the '" << kStageName << "' spec must be an object",
            elem.type() == BSONType::object);

    bool includeCommitTimestamp = false;
    for (auto&& subElem : elem.Obj()) {
        if (subElem.fieldNameStringData() == kIncludeCommitTransactionTimestampFieldName) {
            uassert(6387805,
                    str::stream() << "expected a boolean for the "
                                  << kIncludeCommitTransactionTimestampFieldName << " option to "
                                  << kStageName << " stage, got " << typeName(subElem.type()),
                    subElem.type() == BSONType::boolean);
            includeCommitTimestamp = subElem.Bool();
        } else {
            uasserted(6387800,
                      str::stream() << "unrecognized option to " << kStageName
                                    << " stage: " << subElem.fieldNameStringData());
        }
    }

    return DocumentSourceFindAndModifyImageLookup::create(expCtx, includeCommitTimestamp);
}

DocumentSourceFindAndModifyImageLookup::DocumentSourceFindAndModifyImageLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool includeCommitTimestamp)
    : DocumentSource(kStageName, expCtx),
      _includeCommitTransactionTimestamp(includeCommitTimestamp) {}

StageConstraints DocumentSourceFindAndModifyImageLookup::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kAnyShard,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kDenylist);

    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

Value DocumentSourceFindAndModifyImageLookup::serialize(const SerializationOptions& opts) const {
    return Value(
        Document{{kStageName,
                  Value(Document{{kIncludeCommitTransactionTimestampFieldName,
                                  _includeCommitTransactionTimestamp ? opts.serializeLiteral(true)
                                                                     : Value()}})}});
}

DepsTracker::State DocumentSourceFindAndModifyImageLookup::getDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(std::string{OplogEntry::kSessionIdFieldName});
    deps->fields.insert(std::string{OplogEntry::kTxnNumberFieldName});
    deps->fields.insert(std::string{OplogEntry::kNeedsRetryImageFieldName});
    deps->fields.insert(std::string{OplogEntry::kWallClockTimeFieldName});
    deps->fields.insert(std::string{OplogEntry::kNssFieldName});
    deps->fields.insert(std::string{OplogEntry::kTimestampFieldName});
    deps->fields.insert(std::string{OplogEntry::kTermFieldName});
    deps->fields.insert(std::string{OplogEntry::kUuidFieldName});
    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceFindAndModifyImageLookup::getModifiedPaths() const {
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}
}  // namespace mongo
