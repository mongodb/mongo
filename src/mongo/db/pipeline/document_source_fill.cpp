/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_fill.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(fill,
                                       LiteParsedDocumentSourceDefault::parse,
                                       document_source_fill::createFromBson,
                                       AllowedWithApiStrict::kNeverInVersion1,
                                       AllowedWithClientType::kAny,
                                       nullptr,  // featureFlag
                                       true);
namespace document_source_fill {

std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = FillSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));
    BSONObjBuilder setWindowFieldsSpec;

    // Output object is not optional. Check output field first to maybe skip building the rest of
    // the $setWindowFields spec.
    auto&& outputFields = spec.getOutput();
    uassert(6050203,
            "The output field in '$fill' must contain an object with at least one element",
            outputFields.nFields() > 0);

    BSONObjBuilder setWindowFieldsOutputSpec(setWindowFieldsSpec.subobjStart("output"));
    BSONObjBuilder addFieldsSpec;
    bool needSetWindowFields = false;

    for (const auto& fieldSpec : outputFields) {
        // Each object is a fieldname with a single element object value.
        StringData fieldName = fieldSpec.fieldName();
        uassert(6050200,
                "Each fill output specification must be an object with exactly one field",
                fieldSpec.type() == BSONType::object);
        auto parsedSpec =
            FillOutputSpec::parse(fieldSpec.embeddedObject(), IDLParserContext(kStageName));
        if (auto&& method = parsedSpec.getMethod()) {
            uassert(6050201,
                    "Each fill output specification must have exactly one of 'method' or 'value' "
                    "fields, not both",
                    !parsedSpec.getValue());
            auto&& methodStr = method.value();
            uassert(6050202,
                    str::stream() << "Method must be either " << kLocfMethod << " or "
                                  << kLinearInterpolateMethod,
                    methodStr == kLocfMethod || methodStr == kLinearInterpolateMethod);
            auto&& fullMethodStr =
                methodStr == kLinearInterpolateMethod ? "$" + methodStr + "Fill" : "$" + methodStr;
            setWindowFieldsOutputSpec.append(fieldName, BSON(fullMethodStr << "$" + fieldName));
            needSetWindowFields = true;
        }
        if (auto&& unparsedValueExpr = parsedSpec.getValue()) {
            // Value fields are BSONAnyType.
            auto valueElem = unparsedValueExpr.value().getElement();
            BSONObj fullFieldSpec =
                BSON(fieldName << BSON("$ifNull" << BSON_ARRAY("$" + fieldName << valueElem)));
            addFieldsSpec.appendElements(fullFieldSpec);
        }
    }
    setWindowFieldsOutputSpec.done();

    // Generate sortPattern for $setWindowFields
    if (auto&& sortBy = spec.getSortBy()) {
        if (needSetWindowFields) {
            setWindowFieldsSpec.append("sortBy", sortBy.value());
        } else {
            // sortBy is only required when an output method is used.
            // In the case where a constant value is used for the output
            // the sortBy will be ignored during the rewrite and subsequently
            // never parsed or evaluated. This means we need to perform the parsing
            // in the $fill stage to ensure no invalid values are present.
            SortPattern(*sortBy, pExpCtx);
        }
    }

    if (auto&& partitionByUnparsedExpr = spec.getPartitionBy()) {
        uassert(6050204,
                "Maximum one of 'partitionBy' and 'partitionByFields can be specified in '$fill'",
                !spec.getPartitionByFields());
        auto partitionByField = partitionByUnparsedExpr.value();
        if (std::string* partitionByString = get_if<std::string>(&partitionByField)) {
            if (needSetWindowFields) {
                setWindowFieldsSpec.append("partitionBy", *partitionByString);
            } else {
                ExpressionFieldPath::parse(
                    pExpCtx.get(), *partitionByString, pExpCtx->variablesParseState);
            }
        } else {
            auto&& partitionByMethod = get<BSONObj>(partitionByField);
            if (needSetWindowFields) {
                setWindowFieldsSpec.append("partitionBy", partitionByMethod);
            } else {
                // A partitionBy expression will be ignored during the rewrite if the output is not
                // a method. In that case, the partitionBy expression is never parsed or evaluated,
                // which would allow invalid expressions to go undetected.
                // Therefore, we explicitly parse the partitionBy expression in the $fill stage
                // to ensure that any bogus values are caught early.
                Expression::parseObject(
                    pExpCtx.get(), partitionByMethod, pExpCtx->variablesParseState);
            }
        }
    }

    if (auto&& partitionByFields = spec.getPartitionByFields()) {
        MutableDocument partitionBySpec;
        for (const auto& fieldName : partitionByFields.value()) {
            partitionBySpec.setNestedField(fieldName, Value("$" + fieldName));
        }
        if (needSetWindowFields)
            setWindowFieldsSpec.append("partitionBy", partitionBySpec.freeze().toBson());
    }

    std::list<boost::intrusive_ptr<DocumentSource>> finalSources;
    if (needSetWindowFields) {
        auto finalSpec = BSON("$setWindowFields" << setWindowFieldsSpec.obj());
        finalSources =
            document_source_set_window_fields::createFromBson(finalSpec.firstElement(), pExpCtx);
    }
    auto finalAddFieldsSpec = addFieldsSpec.obj();
    if (finalAddFieldsSpec.nFields() != 0) {
        finalSources.push_back(
            DocumentSourceAddFields::create(std::move(finalAddFieldsSpec), pExpCtx, "$addFields"));
    }

    return finalSources;
}

}  // namespace document_source_fill
}  // namespace mongo
