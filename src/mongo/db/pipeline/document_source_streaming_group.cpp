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

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source_streaming_group.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

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
    return kStageName.rawData();
}

DocumentSource::GetNextResult DocumentSourceStreamingGroup::doGetNext() {
    auto result = _groupProcessor.getNext();
    if (result) {
        return GetNextResult(std::move(*result));
    } else if (_sourceDepleted) {
        dispose();
        return GetNextResult::makeEOF();
    }

    auto prepareResult = readyNextBatch();
    if (prepareResult.isPaused()) {
        return prepareResult;
    }

    result = _groupProcessor.getNext();
    if (!result) {
        return GetNextResult::makeEOF();
    }
    return GetNextResult(std::move(*result));
}

DocumentSourceStreamingGroup::DocumentSourceStreamingGroup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes)
    : DocumentSourceGroupBase(kStageName, expCtx, maxMemoryUsageBytes), _sourceDepleted(false) {}

boost::intrusive_ptr<DocumentSourceStreamingGroup> DocumentSourceStreamingGroup::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    std::vector<size_t> monotonicExpressionIndexes,
    std::vector<AccumulationStatement> accumulationStatements,
    boost::optional<int64_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceStreamingGroup> groupStage =
        new DocumentSourceStreamingGroup(expCtx, maxMemoryUsageBytes);
    groupStage->_groupProcessor.setIdExpression(groupByExpression);
    for (auto&& statement : accumulationStatements) {
        groupStage->_groupProcessor.addAccumulationStatement(statement);
    }
    uassert(7026709,
            "streaming group must have at least one monotonic id expression",
            !monotonicExpressionIndexes.empty());
    uassert(7026710,
            "streaming group monotonic expression indexes must correspond to id expressions",
            std::all_of(monotonicExpressionIndexes.begin(),
                        monotonicExpressionIndexes.end(),
                        [&](size_t i) {
                            return i < groupStage->_groupProcessor.getIdExpressions().size();
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
            monotonicIdFieldsElem.type() == Array);
    const auto& monotonicIdFields = monotonicIdFieldsElem.Array();
    const auto& idFieldNames = groupStage->_groupProcessor.getIdFieldNames();
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
                    fieldNameElem.type() == String);
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
    const auto& idFieldNames = _groupProcessor.getIdFieldNames();
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

DocumentSource::GetNextResult DocumentSourceStreamingGroup::getNextDocument() {
    if (_firstDocumentOfNextBatch) {
        GetNextResult result = std::move(_firstDocumentOfNextBatch.value());
        _firstDocumentOfNextBatch.reset();
        return result;
    }
    return pSource->getNext();
}

DocumentSource::GetNextResult DocumentSourceStreamingGroup::readyNextBatch() {
    _groupProcessor.reset();
    GetNextResult input = getNextDocument();
    return readyNextBatchInner(input);
}

// This separate NOINLINE function is used here to decrease stack utilization of readyNextBatch()
// and prevent stack overflows.
MONGO_COMPILER_NOINLINE DocumentSource::GetNextResult
DocumentSourceStreamingGroup::readyNextBatchInner(GetNextResult input) {
    _groupProcessor.setExecutionStarted();
    // Calculate groups until we either exaust pSource or encounter change in monotonic id
    // expression, which means all current groups are finalized.
    for (; input.isAdvanced(); input = pSource->getNext()) {
        auto root = input.releaseDocument();
        auto groupKey = _groupProcessor.computeGroupKey(root);

        if (isBatchFinished(groupKey)) {
            _firstDocumentOfNextBatch = std::move(root);
            _groupProcessor.readyGroups();
            return input;
        }

        _groupProcessor.add(groupKey, root);
    }

    switch (input.getStatus()) {
        case DocumentSource::GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kPauseExecution: {
            return input;  // Propagate pause.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kEOF: {
            _groupProcessor.readyGroups();
            _sourceDepleted = true;
            return input;
        }
    }
    MONGO_UNREACHABLE;
}

bool DocumentSourceStreamingGroup::isBatchFinished(const Value& id) {
    if (_groupProcessor.getIdExpressions().size() == 1) {
        tassert(7026706,
                "if there are no explicit id fields, it is only one monotonic expression with id 0",
                _monotonicExpressionIndexes.size() == 1 && _monotonicExpressionIndexes[0] == 0);
        return checkForBatchEndAndUpdateLastIdValues([&](size_t) { return id; });
    } else {
        tassert(7026707,
                "if there are explicit id fields, internal representation of id is an array",
                id.isArray());
        const std::vector<Value>& idValues = id.getArray();
        return checkForBatchEndAndUpdateLastIdValues([&](size_t i) { return idValues[i]; });
    }
}

template <typename IdValueGetter>
bool DocumentSourceStreamingGroup::checkForBatchEndAndUpdateLastIdValues(
    const IdValueGetter& idValueGetter) {
    auto assertStreamable = [&](Value value) {
        // Nullish and array values will mess us up because they sort differently than they group.
        // A null and a missing value will compare equal in sorting, but could result in different
        // groups, e.g. {_id: {x: null, y: null}} vs {_id: {}}. An array value will sort by the min
        // or max element, with no tie breaking, but group by the whole array. This means that two
        // of the exact same array could appear in the input sequence, but with a different array in
        // the middle of them, and that would still be considered sorted. That would break our
        // batching group logic.
        uassert(7026708,
                "Monotonic value should not be missing, null or an array",
                !value.nullish() && !value.isArray());
        return value;
    };

    // If _lastMonotonicIdFieldValues is empty, it is the first document, so the only thing we need
    // to do is initialize it.
    if (_lastMonotonicIdFieldValues.empty()) {
        for (size_t i : _monotonicExpressionIndexes) {
            _lastMonotonicIdFieldValues.push_back(assertStreamable(idValueGetter(i)));
        }
        return false;
    } else {
        bool batchFinished = false;
        for (size_t index = 0; index < _monotonicExpressionIndexes.size(); ++index) {
            Value& oldId = _lastMonotonicIdFieldValues[index];
            const Value& id = assertStreamable(idValueGetter(_monotonicExpressionIndexes[index]));
            if (pExpCtx->getValueComparator().compare(oldId, id) != 0) {
                oldId = id;
                batchFinished = true;
            }
        }
        return batchFinished;
    }
}

}  // namespace mongo
