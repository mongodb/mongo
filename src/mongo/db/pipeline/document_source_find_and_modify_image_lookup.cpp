// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalFindAndModifyImageLookup,
                                              FindAndModifyImageLookupLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalFindAndModifyImageLookup,
                                                   DocumentSourceFindAndModifyImageLookup,
                                                   FindAndModifyImageLookupStageParams);

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
                                 HostTypeRequirement::kTargetedShards,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kDenylist);
    constraints.consumesLogicalCollectionData = false;
    constraints.outputDependsOnSingleInput = true;
    return constraints;
}

Value DocumentSourceFindAndModifyImageLookup::serialize(
    const query_shape::SerializationOptions& opts) const {
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
