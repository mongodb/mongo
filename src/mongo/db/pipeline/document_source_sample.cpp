// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_sample.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using boost::intrusive_ptr;

constexpr std::string_view DocumentSourceSample::kStageName;

DocumentSourceSample::DocumentSourceSample(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx), _size(0) {}

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(sample,
                                     SampleLiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(sample, DocumentSourceSample, SampleStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(sample, DocumentSourceSample::id)

Value DocumentSourceSample::serialize(const query_shape::SerializationOptions& opts) const {
    return Value(DOC(kStageName << DOC("size" << opts.serializeLiteral(_size))));
}

intrusive_ptr<DocumentSource> DocumentSourceSample::createFromBson(
    BSONElement specElem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(28745,
            "the $sample stage specification must be an object",
            specElem.type() == BSONType::object);

    bool sizeSpecified = false;
    long long size;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();

        if (fieldName == "size") {
            uassert(28746, "size argument to $sample must be a number", elem.isNumber());
            size = elem.safeNumberLong();
            sizeSpecified = true;
        } else {
            uasserted(28748, str::stream() << "unrecognized option to $sample: " << fieldName);
        }
    }
    uassert(28749, "$sample stage must specify a size", sizeSpecified);

    return DocumentSourceSample::create(expCtx, size);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceSample::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, long long size) {
    uassert(28747, "size argument to $sample must be a positive integer", size > 0);

    intrusive_ptr<DocumentSourceSample> sample(new DocumentSourceSample(expCtx));
    sample->_size = size;
    return sample;
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceSample::distributedPlanLogic(
    const DistributedPlanContext* ctx) {
    // On the merger we need to merge the pre-sorted documents by their random values, then limit to
    // the number we need.
    DistributedPlanLogic logic;
    logic.shardsStage = this;
    if (_size > 0) {
        logic.mergingStages = {DocumentSourceLimit::create(getExpCtx(), _size)};
    }

    // Here we don't use 'randSortSpec' because it uses a metadata sort which the merging logic does
    // not understand. The merging logic will use the serialized sort key, and this sort pattern is
    // just used to communicate ascending/descending information. A pattern like {$meta: "randVal"}
    // is neither ascending nor descending, and so will not be useful when constructing the merging
    // logic.
    logic.mergeSortPattern = BSON("$rand" << -1);
    return logic;
}
}  // namespace mongo
