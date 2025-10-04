/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_sample.h"

#include "mongo/base/string_data.h"
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

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using boost::intrusive_ptr;

constexpr StringData DocumentSourceSample::kStageName;

DocumentSourceSample::DocumentSourceSample(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx), _size(0) {}

REGISTER_DOCUMENT_SOURCE(sample,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSample::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(sample, DocumentSourceSample::id)

Value DocumentSourceSample::serialize(const SerializationOptions& opts) const {
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

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceSample::distributedPlanLogic() {
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
