/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

REGISTER_DOCUMENT_SOURCE_ALIAS(bucket, DocumentSourceBucket::createFromBson);

namespace {
intrusive_ptr<ExpressionConstant> getExpressionConstant(BSONElement expressionElem,
                                                        VariablesParseState vps) {
    auto expr = Expression::parseOperand(expressionElem, vps)->optimize();
    return dynamic_cast<ExpressionConstant*>(expr.get());
}
}  // namespace

vector<intrusive_ptr<DocumentSource>> DocumentSourceBucket::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40201,
            str::stream() << "Argument to $bucket stage must be an object, but found type: "
                          << typeName(elem.type())
                          << ".",
            elem.type() == BSONType::Object);

    const BSONObj bucketObj = elem.embeddedObject();
    BSONObjBuilder groupObjBuilder;
    BSONObjBuilder switchObjBuilder;

    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);

    vector<Value> boundaryValues;
    BSONElement groupByField;
    Value defaultValue;

    bool outputFieldSpecified = false;
    for (auto&& argument : bucketObj) {
        const auto argName = argument.fieldNameStringData();
        if ("groupBy" == argName) {
            groupByField = argument;

            const bool groupByIsExpressionInObject = groupByField.type() == BSONType::Object &&
                groupByField.embeddedObject().firstElementFieldName()[0] == '$';

            const bool groupByIsPrefixedPath =
                groupByField.type() == BSONType::String && groupByField.valueStringData()[0] == '$';
            uassert(40202,
                    str::stream() << "The $bucket 'groupBy' field must be defined as a $-prefixed "
                                     "path or an expression, but found: "
                                  << groupByField.toString(false, false)
                                  << ".",
                    groupByIsExpressionInObject || groupByIsPrefixedPath);
        } else if ("boundaries" == argName) {
            uassert(
                40200,
                str::stream() << "The $bucket 'boundaries' field must be an array, but found type: "
                              << typeName(argument.type())
                              << ".",
                argument.type() == BSONType::Array);

            for (auto&& boundaryElem : argument.embeddedObject()) {
                auto exprConst = getExpressionConstant(boundaryElem, vps);
                uassert(40191,
                        str::stream() << "The $bucket 'boundaries' field must be an array of "
                                         "constant values, but found value: "
                                      << boundaryElem.toString(false, false)
                                      << ".",
                        exprConst);
                boundaryValues.push_back(exprConst->getValue());
            }

            uassert(40192,
                    str::stream()
                        << "The $bucket 'boundaries' field must have at least 2 values, but found "
                        << boundaryValues.size()
                        << " value(s).",
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
                                      << typeName(lower.getType())
                                      << " and "
                                      << typeName(upper.getType())
                                      << ".",
                        lowerCanonicalType == upperCanonicalType);
                // TODO SERVER-25038: This check must be deferred so that it respects the final
                // collator, which is not necessarily the same as the collator at parse time.
                uassert(40194,
                        str::stream()
                            << "The 'boundaries' option to $bucket must be sorted, but elements "
                            << i - 1
                            << " and "
                            << i
                            << " are not in ascending order ("
                            << lower.toString()
                            << " is not less than "
                            << upper.toString()
                            << ").",
                        pExpCtx->getValueComparator().evaluate(lower < upper));
            }
        } else if ("default" == argName) {
            // If there is a default, make sure that it parses to a constant expression then add
            // default to switch.
            auto exprConst = getExpressionConstant(argument, vps);
            uassert(40195,
                    str::stream()
                        << "The $bucket 'default' field must be a constant expression, but found: "
                        << argument.toString(false, false)
                        << ".",
                    exprConst);

            defaultValue = exprConst->getValue();
            defaultValue.addToBsonObj(&switchObjBuilder, "default");
        } else if ("output" == argName) {
            outputFieldSpecified = true;
            uassert(
                40196,
                str::stream() << "The $bucket 'output' field must be an object, but found type: "
                              << typeName(argument.type())
                              << ".",
                argument.type() == BSONType::Object);

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
        //
        // TODO SERVER-25038: This check must be deferred so that it respects the final collator,
        // which is not necessarily the same as the collator at parse time.
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
