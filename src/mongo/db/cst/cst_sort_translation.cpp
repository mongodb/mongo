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

#include <set>
#include <string>
#include <vector>

#include "mongo/db/cst/cst_sort_translation.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/cst/path.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::cst_sort_translation {

SortPattern translateSortSpec(const CNode& cst,
                              const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // Assume object children, only thing possible for sort.
    const auto& children = cst.objectChildren();
    std::vector<SortPattern::SortPatternPart> sortKeys;
    for (const auto& keyValPair : children) {
        auto&& path = path::vectorToString(
            std::move(stdx::get<SortPath>(stdx::get<FieldnamePath>(keyValPair.first)).components));
        stdx::visit(
            OverloadedVisitor{
                [&](const CNode::ObjectChildren& object) {
                    // $meta is always the only key in the object, and always has a KeyValue as its
                    // value.
                    // If sorting by textScore, put highest scores first. If $meta was specified
                    // with randVal order doesn't matter, so always put descending.
                    auto keyVal = stdx::get<KeyValue>(object[0].second.payload);
                    switch (keyVal) {
                        case KeyValue::randVal:
                            sortKeys.push_back(SortPattern::SortPatternPart{
                                false,
                                boost::none,
                                make_intrusive<ExpressionMeta>(expCtx.get(),
                                                               DocumentMetadataFields::kRandVal)});
                            break;
                        case KeyValue::textScore:
                            sortKeys.push_back(SortPattern::SortPatternPart{
                                false,
                                boost::none,
                                make_intrusive<ExpressionMeta>(
                                    expCtx.get(), DocumentMetadataFields::kTextScore)});
                            break;
                        default:
                            MONGO_UNREACHABLE;
                    }
                },
                [&](const KeyValue& keyValue) {
                    switch (keyValue) {
                        case KeyValue::intOneKey:
                        case KeyValue::longOneKey:
                        case KeyValue::doubleOneKey:
                        case KeyValue::decimalOneKey:
                            sortKeys.push_back(SortPattern::SortPatternPart{
                                true, FieldPath{std::move(path)}, nullptr /* meta */});

                            break;
                        case KeyValue::intNegOneKey:
                        case KeyValue::longNegOneKey:
                        case KeyValue::doubleNegOneKey:
                        case KeyValue::decimalNegOneKey:
                            sortKeys.push_back(SortPattern::SortPatternPart{
                                false, FieldPath{std::move(path)}, nullptr /* meta */});
                            break;
                        default:
                            MONGO_UNREACHABLE;
                    }
                },
                [](auto&&) { MONGO_UNREACHABLE; },
            },
            keyValPair.second.payload);
    }
    return SortPattern(std::move(sortKeys));
}

}  // namespace mongo::cst_sort_translation
