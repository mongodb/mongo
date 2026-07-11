// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(fill,
                                     FillLiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_CONTAINER_WITH_STAGE_PARAMS_DEFAULT(fill,
                                                             document_source_fill,
                                                             FillStageParams);

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
        std::string_view fieldName = fieldSpec.fieldName();
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
            auto&& fullMethodStr = methodStr == kLinearInterpolateMethod
                ? "$" + std::string{methodStr} + "Fill"
                : "$" + std::string{methodStr};
            setWindowFieldsOutputSpec.append(fieldName,
                                             BSON(fullMethodStr << "$" + std::string{fieldName}));
            needSetWindowFields = true;
        }
        if (auto&& unparsedValueExpr = parsedSpec.getValue()) {
            // Value fields are BSONAnyType.
            auto valueElem = unparsedValueExpr.value().getElement();
            BSONObj fullFieldSpec =
                BSON(fieldName << BSON("$ifNull"
                                       << BSON_ARRAY("$" + std::string{fieldName} << valueElem)));
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
            partitionBySpec.setNestedField(fieldName, Value("$" + std::string{fieldName}));
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
