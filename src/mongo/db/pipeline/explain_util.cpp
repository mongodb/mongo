/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/explain_util.h"

#include "mongo/db/exec/document_value/document.h"

namespace mongo {

std::vector<Value> mergeExplains(const Pipeline& p1,
                                 const exec::agg::Pipeline& p2,
                                 const SerializationOptions& opts) {
    std::vector<Value> result;
    auto e1 = p1.writeExplainOps(opts);
    auto e2 = p2.writeExplainOps(opts);
    tassert(10422601, "Pipelines are not equal", e1.size() == e2.size());
    result.reserve(e1.size());

    for (size_t i = 0; i < e1.size(); i++) {
        tassert(
            10422602, "Expected explain input of type object", e1[i].getType() == BSONType::object);
        tassert(
            10422603, "Expected explain input of type object", e2[i].getType() == BSONType::object);
        Document d1 = e1[i].getDocument();
        Document d2 = e2[i].getDocument();
        result.emplace_back(Document::merge(d1, d2));
    }

    return result;
}

}  // namespace mongo
