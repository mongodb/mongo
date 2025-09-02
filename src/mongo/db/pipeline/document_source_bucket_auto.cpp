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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/smart_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator_for_bucket_auto.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cmath>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::string;
using std::vector;

REGISTER_DOCUMENT_SOURCE(bucketAuto,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceBucketAuto::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(bucketAuto, DocumentSourceBucketAuto::id)

namespace {

boost::intrusive_ptr<Expression> parseGroupByExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONElement& groupByField,
    const VariablesParseState& vps) {
    if (groupByField.type() == BSONType::object &&
        groupByField.embeddedObject().firstElementFieldName()[0] == '$') {
        return Expression::parseObject(expCtx.get(), groupByField.embeddedObject(), vps);
    } else if (groupByField.type() == BSONType::string &&
               // Lager than 2 because we need a '$', at least one char for the field name and
               // the final terminating 0.
               groupByField.valuestrsize() > 2 && groupByField.valueStringData()[0] == '$') {
        return ExpressionFieldPath::parse(expCtx.get(), groupByField.str(), vps);
    }
    uasserted(40239,
              str::stream() << "The $bucketAuto 'groupBy' field must be defined as a $-prefixed "
                               "path or an expression object, but found: "
                            << groupByField.toString(false, false));
}

}  // namespace

const char* DocumentSourceBucketAuto::getSourceName() const {
    return kStageName.data();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceBucketAuto::optimize() {
    _groupByExpression = _groupByExpression->optimize();
    for (auto&& accumulatedField : *_accumulatedFields) {
        accumulatedField.expr.argument = accumulatedField.expr.argument->optimize();
        accumulatedField.expr.initializer = accumulatedField.expr.initializer->optimize();
    }
    return this;
}

DepsTracker::State DocumentSourceBucketAuto::getDependencies(DepsTracker* deps) const {
    expression::addDependencies(_groupByExpression.get(), deps);

    for (auto&& accumulatedField : *_accumulatedFields) {
        // Anything the per-doc expression depends on, the whole stage depends on.
        expression::addDependencies(accumulatedField.expr.argument.get(), deps);
        // The initializer should be an ExpressionConstant, or something that optimizes to one.
        // ExpressionConstant doesn't have dependencies.
    }

    // We know exactly which fields will be present in the output document. Future stages cannot
    // depend on any further fields. The grouping process will remove any metadata from the
    // documents, so there can be no further dependencies on metadata.
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

void DocumentSourceBucketAuto::addVariableRefs(std::set<Variables::Id>* refs) const {
    expression::addVariableRefs(_groupByExpression.get(), refs);

    for (auto&& accumulatedField : *_accumulatedFields) {
        expression::addVariableRefs(accumulatedField.expr.argument.get(), refs);
        // The initializer should be an ExpressionConstant, or something that optimizes to one.
        // ExpressionConstant doesn't have dependencies.
    }
}

Value DocumentSourceBucketAuto::serialize(const SerializationOptions& opts) const {
    MutableDocument insides;

    insides["groupBy"] = _groupByExpression->serialize(opts);
    insides["buckets"] = opts.serializeLiteral(_nBuckets);

    if (_granularityRounder) {
        //"granularity" only supports some strings, so a specific representative value is used if
        // necessary.
        insides["granularity"] =
            opts.serializeLiteral(_granularityRounder->getName(), Value("R5"_sd));
    }

    MutableDocument outputSpec(_accumulatedFields->size());
    for (auto&& accumulatedField : *_accumulatedFields) {
        intrusive_ptr<AccumulatorState> accum = accumulatedField.makeAccumulator();
        outputSpec[opts.serializeFieldPathFromString(accumulatedField.fieldName)] =
            Value(accum->serialize(
                accumulatedField.expr.initializer, accumulatedField.expr.argument, opts));
    }

    insides["output"] = outputSpec.freezeToValue();

    MutableDocument out;
    out[getSourceName()] = insides.freezeToValue();

    return out.freezeToValue();
}

intrusive_ptr<DocumentSourceBucketAuto> DocumentSourceBucketAuto::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    int numBuckets,
    std::vector<AccumulationStatement> accumulationStatements,
    const boost::intrusive_ptr<GranularityRounder>& granularityRounder) {
    uassert(40243,
            str::stream() << "The $bucketAuto 'buckets' field must be greater than 0, but found: "
                          << numBuckets,
            numBuckets > 0);
    // If there is no output field specified, then add the default one.
    if (accumulationStatements.empty()) {
        accumulationStatements.emplace_back(
            "count",
            AccumulationExpression(
                ExpressionConstant::create(pExpCtx.get(), Value(BSONNULL)),
                ExpressionConstant::create(pExpCtx.get(), Value(1)),
                [pExpCtx] { return make_intrusive<AccumulatorSum>(pExpCtx.get()); },
                AccumulatorSum::kName));
    }
    return new DocumentSourceBucketAuto(pExpCtx,
                                        groupByExpression,
                                        numBuckets,
                                        std::move(accumulationStatements),
                                        granularityRounder);
}

DocumentSourceBucketAuto::DocumentSourceBucketAuto(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    int numBuckets,
    std::vector<AccumulationStatement> accumulationStatements,
    const boost::intrusive_ptr<GranularityRounder>& granularityRounder)
    : DocumentSource(kStageName, pExpCtx),
      _accumulatedFields{std::make_shared<std::vector<AccumulationStatement>>()},
      _populated{std::make_shared<bool>(false)},
      _groupByExpression(groupByExpression),
      _granularityRounder(granularityRounder),
      _nBuckets(numBuckets) {
    invariant(!accumulationStatements.empty());
    _accumulatedFields->reserve(accumulationStatements.size());
    for (auto&& accumulationStatement : accumulationStatements) {
        _accumulatedFields->push_back(accumulationStatement);
    }
}

boost::intrusive_ptr<Expression> DocumentSourceBucketAuto::getGroupByExpression() const {
    return _groupByExpression;
}

boost::intrusive_ptr<Expression>& DocumentSourceBucketAuto::getMutableGroupByExpression() {
    tassert(7020501,
            "Cannot change group by expressions once execution has begun in BucketAuto",
            !*_populated);
    return _groupByExpression;
}

const std::vector<AccumulationStatement>& DocumentSourceBucketAuto::getAccumulationStatements()
    const {
    return *_accumulatedFields;
}

std::vector<AccumulationStatement>& DocumentSourceBucketAuto::getMutableAccumulationStatements() {
    tassert(7020502,
            "Cannot change accumulated field expression once execution has begun in BucketAuto",
            !*_populated);
    return *_accumulatedFields;
}

intrusive_ptr<DocumentSource> DocumentSourceBucketAuto::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40240,
            str::stream() << "The argument to $bucketAuto must be an object, but found type: "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    VariablesParseState vps = pExpCtx->variablesParseState;
    vector<AccumulationStatement> accumulationStatements;
    boost::intrusive_ptr<Expression> groupByExpression;
    boost::optional<int> numBuckets;
    boost::intrusive_ptr<GranularityRounder> granularityRounder;

    pExpCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();
        if ("groupBy" == argName) {
            groupByExpression = parseGroupByExpression(pExpCtx, argument, vps);
        } else if ("buckets" == argName) {
            Value bucketsValue = Value(argument);

            uassert(
                40241,
                str::stream()
                    << "The $bucketAuto 'buckets' field must be a numeric value, but found type: "
                    << typeName(argument.type()),
                bucketsValue.numeric());

            uassert(40242,
                    str::stream() << "The $bucketAuto 'buckets' field must be representable as a "
                                     "32-bit integer, but found "
                                  << Value(argument).coerceToDouble(),
                    bucketsValue.integral());

            numBuckets = bucketsValue.coerceToInt();
        } else if ("output" == argName) {
            uassert(40244,
                    str::stream()
                        << "The $bucketAuto 'output' field must be an object, but found type: "
                        << typeName(argument.type()),
                    argument.type() == BSONType::object);

            for (auto&& outputField : argument.embeddedObject()) {
                auto parsedStmt = AccumulationStatement::parseAccumulationStatement(
                    pExpCtx.get(), outputField, vps);
                auto stmt =
                    replaceAccumulationStatementForBucketAuto(pExpCtx.get(), std::move(parsedStmt));
                stmt.expr.initializer = stmt.expr.initializer->optimize();
                uassert(4544714,
                        "Can't refer to the group key in $bucketAuto",
                        ExpressionConstant::isNullOrConstant(stmt.expr.initializer));
                accumulationStatements.push_back(std::move(stmt));
            }
        } else if ("granularity" == argName) {
            uassert(40261,
                    str::stream()
                        << "The $bucketAuto 'granularity' field must be a string, but found type: "
                        << typeName(argument.type()),
                    argument.type() == BSONType::string);
            granularityRounder = GranularityRounder::getGranularityRounder(pExpCtx, argument.str());
        } else {
            uasserted(40245, str::stream() << "Unrecognized option to $bucketAuto: " << argName);
        }
    }

    uassert(40246,
            "$bucketAuto requires 'groupBy' and 'buckets' to be specified",
            groupByExpression && numBuckets);

    return DocumentSourceBucketAuto::create(pExpCtx,
                                            groupByExpression,
                                            numBuckets.value(),
                                            std::move(accumulationStatements),
                                            granularityRounder);
}

}  // namespace mongo
