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

#include "mongo/db/pipeline/document_source_bucket.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::list;
using std::vector;

REGISTER_DOCUMENT_SOURCE(bucket,
                         DocumentSourceBucket::LiteParsed::parse,
                         DocumentSourceBucket::createFromBson,
                         AllowedWithApiStrict::kAlways);

namespace {
intrusive_ptr<ExpressionConstant> getExpressionConstant(ExpressionContext* const expCtx,
                                                        BSONElement expressionElem,
                                                        const VariablesParseState& vps) {
    auto expr = Expression::parseOperand(expCtx, expressionElem, vps)->optimize();
    return dynamic_cast<ExpressionConstant*>(expr.get());
}
}  // namespace

list<intrusive_ptr<DocumentSource>> DocumentSourceBucket::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40201,
            str::stream() << "Argument to $bucket stage must be an object, but found type: "
                          << typeName(elem.type()) << ".",
            elem.type() == BSONType::object);

    const BSONObj bucketObj = elem.embeddedObject();
    BSONObjBuilder groupObjBuilder;
    BSONObjBuilder switchObjBuilder;

    VariablesParseState vps = pExpCtx->variablesParseState;

    vector<Value> boundaryValues;
    BSONElement groupByField;
    Value defaultValue;

    bool outputFieldSpecified = false;
    for (auto&& argument : bucketObj) {
        const auto argName = argument.fieldNameStringData();
        if ("groupBy" == argName) {
            groupByField = argument;

            const bool groupByIsExpressionInObject = groupByField.type() == BSONType::object &&
                groupByField.embeddedObject().firstElementFieldNameStringData().starts_with('$');

            const bool groupByIsPrefixedPath = groupByField.type() == BSONType::string &&
                groupByField.valueStringData().starts_with('$');
            uassert(40202,
                    str::stream() << "The $bucket 'groupBy' field must be defined as a $-prefixed "
                                     "path or an expression, but found: "
                                  << groupByField.toString(false, false) << ".",
                    groupByIsExpressionInObject || groupByIsPrefixedPath);
        } else if ("boundaries" == argName) {
            uassert(
                40200,
                str::stream() << "The $bucket 'boundaries' field must be an array, but found type: "
                              << typeName(argument.type()) << ".",
                argument.type() == BSONType::array);

            for (auto&& boundaryElem : argument.embeddedObject()) {
                auto exprConst = getExpressionConstant(pExpCtx.get(), boundaryElem, vps);
                uassert(40191,
                        str::stream() << "The $bucket 'boundaries' field must be an array of "
                                         "constant values, but found value: "
                                      << boundaryElem.toString(false, false) << ".",
                        exprConst);
                boundaryValues.push_back(exprConst->getValue());
            }

            uassert(40192,
                    str::stream()
                        << "The $bucket 'boundaries' field must have at least 2 values, but found "
                        << boundaryValues.size() << " value(s).",
                    boundaryValues.size() >= 2);

            // Make sure that the boundaries are unique, sorted in ascending order, and have the
            // same canonical type.
            for (size_t i = 1; i < boundaryValues.size(); ++i) {
                Value lower = boundaryValues[i - 1];
                Value upper = boundaryValues[i];
                int lowerCanonicalType = canonicalizeBSONType(lower.getType());
                int upperCanonicalType = canonicalizeBSONType(upper.getType());

                uassert(40193,
                        str::stream() << "All values in the the 'boundaries' option to $bucket "
                                         "must have the same type. Found conflicting types "
                                      << typeName(lower.getType()) << " and "
                                      << typeName(upper.getType()) << ".",
                        lowerCanonicalType == upperCanonicalType);
                uassert(40194,
                        str::stream()
                            << "The 'boundaries' option to $bucket must be sorted, but elements "
                            << i - 1 << " and " << i << " are not in ascending order ("
                            << lower.toString() << " is not less than " << upper.toString() << ").",
                        pExpCtx->getValueComparator().evaluate(lower < upper));
            }
        } else if ("default" == argName) {
            // If there is a default, make sure that it parses to a constant expression then add
            // default to switch.
            auto exprConst = getExpressionConstant(pExpCtx.get(), argument, vps);
            uassert(40195,
                    str::stream()
                        << "The $bucket 'default' field must be a constant expression, but found: "
                        << argument.toString(false, false) << ".",
                    exprConst);

            defaultValue = exprConst->getValue();
            defaultValue.addToBsonObj(&switchObjBuilder, "default");
        } else if ("output" == argName) {
            outputFieldSpecified = true;
            uassert(
                40196,
                str::stream() << "The $bucket 'output' field must be an object, but found type: "
                              << typeName(argument.type()) << ".",
                argument.type() == BSONType::object);

            for (auto&& outputElem : argument.embeddedObject()) {
                groupObjBuilder.append(outputElem);
            }
        } else {
            uasserted(40197, str::stream() << "Unrecognized option to $bucket: " << argName << ".");
        }
    }

    const bool isMissingRequiredField = groupByField.eoo() || boundaryValues.empty();
    uassert(40198,
            "$bucket requires 'groupBy' and 'boundaries' to be specified.",
            !isMissingRequiredField);

    Value lowerValue = boundaryValues.front();
    Value upperValue = boundaryValues.back();
    if (canonicalizeBSONType(defaultValue.getType()) ==
        canonicalizeBSONType(lowerValue.getType())) {
        // If the default has the same canonical type as the bucket's boundaries, then make sure the
        // default is less than the lowest boundary or greater than or equal to the highest
        // boundary.
        const auto& valueCmp = pExpCtx->getValueComparator();
        const bool hasValidDefault = valueCmp.evaluate(defaultValue < lowerValue) ||
            valueCmp.evaluate(defaultValue >= upperValue);
        uassert(40199,
                "The $bucket 'default' field must be less than the lowest boundary or greater than "
                "or equal to the highest boundary.",
                hasValidDefault);
    }

    // Make the branches for the $switch expression.
    BSONArrayBuilder branchesBuilder;
    for (size_t i = 1; i < boundaryValues.size(); ++i) {
        Value lower = boundaryValues[i - 1];
        Value upper = boundaryValues[i];
        BSONObj caseExpr =
            BSON("$and" << BSON_ARRAY(BSON("$gte" << BSON_ARRAY(groupByField << lower))
                                      << BSON("$lt" << BSON_ARRAY(groupByField << upper))));
        branchesBuilder.append(BSON("case" << caseExpr << "then" << lower));
    }

    // Add the $switch expression to the group BSON object.
    switchObjBuilder.append("branches", branchesBuilder.arr());
    groupObjBuilder.append("_id", BSON("$switch" << switchObjBuilder.obj()));

    // If no output is specified, add a count field by default.
    if (!outputFieldSpecified) {
        groupObjBuilder.append("count", BSON("$sum" << 1));
    }

    BSONObj groupObj = BSON("$group" << groupObjBuilder.obj());
    BSONObj sortObj = BSON("$sort" << BSON("_id" << 1));

    auto groupSource = DocumentSourceGroup::createFromBson(groupObj.firstElement(), pExpCtx);
    auto sortSource = DocumentSourceSort::createFromBson(sortObj.firstElement(), pExpCtx);

    return {groupSource, sortSource};
}
}  // namespace mongo
