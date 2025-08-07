/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionMeta& expr, const Document& root, Variables* variables) {
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
               Variables* variables) {
    return root.metadata().getSortKey();
}

Value evaluate(const ExpressionType& expr, const Document& root, Variables* variables) {
    Value val(expr.getChildren()[0]->evaluate(root, variables));
    return Value(StringData(typeName(val.getType())));
}

Value evaluate(const ExpressionSubtype& expr, const Document& root, Variables* variables) {
    Value val(expr.getChildren()[0]->evaluate(root, variables));
    if (val.nullish()) {
        return Value(BSONNULL);
    }

    uassert(10389300,
            str::stream() << "Provided value: " << val.toString()
                          << ", does not have a subtype, but $subtype was used.",
            val.getType() == BSONType::binData);

    return Value(static_cast<int>(val.getBinData().type));
}

Value evaluate(const ExpressionTestApiVersion& expr, const Document& root, Variables* variables) {
    return Value(1);
}

Value evaluate(const ExpressionLet& expr, const Document& root, Variables* variables) {
    for (const auto& item : expr.getVariableMap()) {
        // It is guaranteed at parse-time that these expressions don't use the variable ids we
        // are setting
        variables->setValue(item.first, item.second.expression->evaluate(root, variables));
    }

    return expr.getSubExpression()->evaluate(root, variables);
}

Value evaluate(const ExpressionTestFeatureFlags& expr, const Document& root, Variables* variables) {
    return Value(1);
}

}  // namespace exec::expression
}  // namespace mongo
