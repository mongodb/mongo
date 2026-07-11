// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/explain_util.h"

#include "mongo/db/exec/document_value/document.h"

namespace mongo {

std::vector<Value> mergeExplains(const Pipeline& p1,
                                 const exec::agg::Pipeline& p2,
                                 const query_shape::SerializationOptions& opts) {
    auto e1 = p1.writeExplainOps(opts);
    auto e2 = p2.writeExplainOps(opts);
    return mergeExplains(e1, e2);
}

std::vector<Value> mergeExplains(const std::vector<Value>& lhs, const std::vector<Value>& rhs) {
    tassert(10422601, "pipeline sizes are not equal", lhs.size() == rhs.size());

    std::vector<Value> result;
    result.reserve(lhs.size());

    for (size_t i = 0; i < lhs.size(); i++) {
        tassert(10422602,
                "expected explain input of type object",
                lhs[i].getType() == BSONType::object);
        tassert(10422603,
                "expected explain input of type object",
                rhs[i].getType() == BSONType::object);
        Document d1 = lhs[i].getDocument();
        Document d2 = rhs[i].getDocument();
        result.emplace_back(Document::deepMerge(d1, d2));
    }

    return result;
}

}  // namespace mongo
