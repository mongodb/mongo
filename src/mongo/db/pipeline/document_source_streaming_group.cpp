/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_streaming_group.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/*
 * $_internalStreamingGroup is an internal stage that is only used in certain cases by the
 * pipeline optimizer. For now it should not be used anywhere outside the MongoDB server.
 */
REGISTER_DOCUMENT_SOURCE(_internalStreamingGroup,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceStreamingGroup::createFromBson,
                         AllowedWithApiStrict::kAlways);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalStreamingGroup, DocumentSourceStreamingGroup::id)

constexpr StringData DocumentSourceStreamingGroup::kStageName;

const char* DocumentSourceStreamingGroup::getSourceName() const {
    return kStageName.data();
}

DocumentSourceStreamingGroup::DocumentSourceStreamingGroup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes)
    : DocumentSourceGroupBase(kStageName, expCtx, maxMemoryUsageBytes) {}

boost::intrusive_ptr<DocumentSourceStreamingGroup> DocumentSourceStreamingGroup::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    std::vector<size_t> monotonicExpressionIndexes,
    std::vector<AccumulationStatement> accumulationStatements,
    boost::optional<int64_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceStreamingGroup> groupStage =
        new DocumentSourceStreamingGroup(expCtx, maxMemoryUsageBytes);
    groupStage->_groupProcessor->setIdExpression(groupByExpression);
    for (auto&& statement : accumulationStatements) {
        groupStage->_groupProcessor->addAccumulationStatement(statement);
    }
    uassert(7026709,
            "streaming group must have at least one monotonic id expression",
            !monotonicExpressionIndexes.empty());
    uassert(7026710,
            "streaming group monotonic expression indexes must correspond to id expressions",
            std::all_of(monotonicExpressionIndexes.begin(),
                        monotonicExpressionIndexes.end(),
                        [&](size_t i) {
                            return i < groupStage->_groupProcessor->getIdExpressions().size();
                        }));
    groupStage->_monotonicExpressionIndexes = std::move(monotonicExpressionIndexes);
    return groupStage;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceStreamingGroup::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return createFromBsonWithMaxMemoryUsage(std::move(elem), expCtx, boost::none);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceStreamingGroup::createFromBsonWithMaxMemoryUsage(
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceStreamingGroup> groupStage =
        new DocumentSourceStreamingGroup(expCtx, maxMemoryUsageBytes);
    groupStage->initializeFromBson(elem);

    // BSONObj must outlive BSONElement. See BSONElement, BSONObj::getField().
    auto obj = elem.Obj();
    const auto& monotonicIdFieldsElem = obj.getField(kMonotonicIdFieldsSpecField);
    uassert(7026702,
            "streaming group must specify an array of monotonic id fields " +
                kMonotonicIdFieldsSpecField,
            monotonicIdFieldsElem.type() == BSONType::array);
    const auto& monotonicIdFields = monotonicIdFieldsElem.Array();
    const auto& idFieldNames = groupStage->_groupProcessor->getIdFieldNames();
    if (idFieldNames.empty()) {
        uassert(7026703,
                "if there is no explicit id fields, " + kMonotonicIdFieldsSpecField +
                    " must contain a single \"_id\" string",
                monotonicIdFields.size() == 1 &&
                    monotonicIdFields[0].valueStringDataSafe() == "_id"_sd);
        groupStage->_monotonicExpressionIndexes.push_back(0);
    } else {
        groupStage->_monotonicExpressionIndexes.reserve(monotonicIdFields.size());
        for (const auto& fieldNameElem : monotonicIdFields) {
            uassert(7026704,
                    kMonotonicIdFieldsSpecField + " elements must be strings",
                    fieldNameElem.type() == BSONType::string);
            StringData fieldName = fieldNameElem.valueStringData();
            auto it = std::find(idFieldNames.begin(), idFieldNames.end(), fieldName);
            uassert(7026705, "id field not found", it != idFieldNames.end());
            groupStage->_monotonicExpressionIndexes.push_back(
                std::distance(idFieldNames.begin(), it));
        }
        std::sort(groupStage->_monotonicExpressionIndexes.begin(),
                  groupStage->_monotonicExpressionIndexes.end());
    }

    return groupStage;
}

void DocumentSourceStreamingGroup::serializeAdditionalFields(
    MutableDocument& out, const SerializationOptions& opts) const {
    std::vector<Value> monotonicIdFields;
    const auto& idFieldNames = _groupProcessor->getIdFieldNames();
    if (idFieldNames.empty()) {
        monotonicIdFields.emplace_back(opts.serializeFieldPath("_id"));
    } else {
        for (size_t i : _monotonicExpressionIndexes) {
            monotonicIdFields.emplace_back(opts.serializeFieldPathFromString(idFieldNames[i]));
        }
    }
    out[kMonotonicIdFieldsSpecField] = Value(std::move(monotonicIdFields));
}

bool DocumentSourceStreamingGroup::isSpecFieldReserved(StringData fieldName) {
    return fieldName == kMonotonicIdFieldsSpecField;
}

}  // namespace mongo
