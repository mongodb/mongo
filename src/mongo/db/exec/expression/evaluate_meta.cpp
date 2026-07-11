// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"

#include <string_view>

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionMeta& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    const auto& metadata = root.metadata();
    switch (expr.getMetaType()) {
        case DocumentMetadataFields::MetaType::kScore:
            return metadata.hasScore() ? Value(metadata.getScore()) : Value();
        case DocumentMetadataFields::MetaType::kVectorSearchScore:
            return metadata.hasVectorSearchScore() ? Value(metadata.getVectorSearchScore())
                                                   : Value();
        case DocumentMetadataFields::MetaType::kTextScore:
            return metadata.hasTextScore() ? Value(metadata.getTextScore()) : Value();
        case DocumentMetadataFields::MetaType::kRandVal:
            return metadata.hasRandVal() ? Value(metadata.getRandVal()) : Value();
        case DocumentMetadataFields::MetaType::kSearchScore:
            return metadata.hasSearchScore() ? Value(metadata.getSearchScore()) : Value();
        case DocumentMetadataFields::MetaType::kSearchHighlights:
            return metadata.hasSearchHighlights() ? Value(metadata.getSearchHighlights()) : Value();
        case DocumentMetadataFields::MetaType::kGeoNearDist:
            return metadata.hasGeoNearDistance() ? Value(metadata.getGeoNearDistance()) : Value();
        case DocumentMetadataFields::MetaType::kGeoNearPoint:
            return metadata.hasGeoNearPoint() ? Value(metadata.getGeoNearPoint()) : Value();
        case DocumentMetadataFields::MetaType::kRecordId: {
            // Be sure that a RecordId can be represented by a long long.
            static_assert(RecordId::kMinRepr >= std::numeric_limits<long long>::min());
            static_assert(RecordId::kMaxRepr <= std::numeric_limits<long long>::max());
            if (!metadata.hasRecordId()) {
                return Value();
            }

            BSONObjBuilder builder;
            metadata.getRecordId().serializeToken("", &builder);
            return Value(builder.done().firstElement());
        }
        case DocumentMetadataFields::MetaType::kIndexKey:
            return metadata.hasIndexKey() ? Value(metadata.getIndexKey()) : Value();
        case DocumentMetadataFields::MetaType::kSortKey:
            return metadata.hasSortKey()
                ? Value(DocumentMetadataFields::serializeSortKey(metadata.isSingleElementKey(),
                                                                 metadata.getSortKey()))
                : Value();
        case DocumentMetadataFields::MetaType::kSearchScoreDetails:
            return metadata.hasSearchScoreDetails() ? Value(metadata.getSearchScoreDetails())
                                                    : Value();
        case DocumentMetadataFields::MetaType::kSearchRootDocumentId:
            return metadata.hasSearchRootDocumentId() ? Value(metadata.getSearchRootDocumentId())
                                                      : Value();
        case DocumentMetadataFields::MetaType::kSearchSequenceToken:
            return metadata.hasSearchSequenceToken() ? Value(metadata.getSearchSequenceToken())
                                                     : Value();
        case DocumentMetadataFields::MetaType::kTimeseriesBucketMinTime:
            return metadata.hasTimeseriesBucketMinTime()
                ? Value(metadata.getTimeseriesBucketMinTime())
                : Value();
        case DocumentMetadataFields::MetaType::kTimeseriesBucketMaxTime:
            return metadata.hasTimeseriesBucketMaxTime()
                ? Value(metadata.getTimeseriesBucketMaxTime())
                : Value();
        case DocumentMetadataFields::MetaType::kScoreDetails:
            return metadata.hasScoreDetails() ? metadata.getScoreDetails() : Value();
        case DocumentMetadataFields::MetaType::kStream:
            return metadata.hasStream() ? metadata.getStream() : Value();
        case DocumentMetadataFields::MetaType::kChangeStreamControlEvent:
            return Value(metadata.isChangeStreamControlEvent());

        default:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

Value evaluate(const ExpressionInternalRawSortKey& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    return root.metadata().getSortKey();
}

Value evaluate(const ExpressionType& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    Value val(expr.getChildren()[0]->evaluate(root, variables, ctx));
    return Value(std::string_view(typeName(val.getType())));
}

Value evaluate(const ExpressionSubtype& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    Value val(expr.getChildren()[0]->evaluate(root, variables, ctx));
    if (val.nullish()) {
        return Value(BSONNULL);
    }

    uassert(10389300,
            str::stream() << "Provided value: " << val.toString()
                          << ", does not have a subtype, but $subtype was used.",
            val.getType() == BSONType::binData);

    return Value(static_cast<int>(val.getBinData().type));
}

Value evaluate(const ExpressionTestApiVersion& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    return Value(1);
}

Value evaluate(const ExpressionLet& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    for (const auto& item : expr.getVariableMap()) {
        // It is guaranteed at parse-time that these expressions don't use the variable ids we
        // are setting
        variables->setValue(item.first, item.second.expression->evaluate(root, variables, ctx));
    }

    return expr.getSubExpression()->evaluate(root, variables, ctx);
}

Value evaluate(const ExpressionTestFeatureFlags& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    return Value(1);
}

}  // namespace exec::expression
}  // namespace mongo
