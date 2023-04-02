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

#include "mongo/db/timeseries/timeseries_update_delete_util.h"

#include "mongo/db/exec/bucket_unpacker.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"

namespace mongo::timeseries {
namespace {

/**
 * Returns whether the given metaField is the first element of the dotted path in the given
 * field.
 */
bool isMetaFieldFirstElementOfDottedPathField(StringData field, StringData metaField) {
    return field.substr(0, field.find('.')) == metaField;
}

/**
 * Returns a string where the substring leading up to "." in the given field is replaced with
 * "meta". If there is no "." in the given field, returns "meta".
 */
std::string getRenamedField(StringData field) {
    size_t dotIndex = field.find('.');
    return dotIndex != std::string::npos ? "meta" + field.substr(dotIndex, field.size() - dotIndex)
                                         : "meta";
}

void assertQueryFieldIsMetaField(bool isMetaField, StringData metaField) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("Cannot perform an update or delete on a time-series collection "
                        "when querying on a field that is not the metaField '{}'",
                        metaField),
            isMetaField);
}

void assertUpdateFieldIsMetaField(bool isMetaField, StringData metaField) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("Cannot perform an update on a time-series collection which updates a "
                        "field that is not the metaField '{}'",
                        metaField),
            isMetaField);
}

/**
 * Recurses through the mutablebson element query and replaces any occurrences of the metaField with
 * "meta" accounting for queries that may be in dot notation. isTopLevelField is set for elements
 * which contain a top-level field name, and parentIsArray is set for elements with an array as a
 * parent.
 */
void replaceQueryMetaFieldName(mutablebson::Element elem,
                               StringData metaField,
                               bool isTopLevelField = true,
                               bool parentIsArray = false) {
    auto fieldName = elem.getFieldName();

    uassert(ErrorCodes::InvalidOptions,
            "Cannot use $expr when performing an update or delete on a time-series collection",
            fieldName != "$expr");

    uassert(ErrorCodes::InvalidOptions,
            "Cannot use $where when performing an update or delete on a time-series collection",
            fieldName != "$where");

    // Replace any occurences of the metaField in the top-level required fields of the JSON Schema
    // object with "meta".
    if (fieldName == "$jsonSchema") {
        mutablebson::Element requiredElem = elem.findFirstChildNamed("required");
        if (requiredElem.ok()) {
            for (auto subElem = requiredElem.leftChild(); subElem.ok();
                 subElem = subElem.rightSibling()) {
                assertQueryFieldIsMetaField(subElem.isType(BSONType::String) &&
                                                subElem.getValueString() == metaField,
                                            metaField);
                invariantStatusOK(
                    subElem.setValueString(getRenamedField(subElem.getValueString())));
            }
        }

        mutablebson::Element propertiesElem = elem.findFirstChildNamed("properties");
        if (propertiesElem.ok()) {
            for (auto property = propertiesElem.leftChild(); property.ok();
                 property = property.rightSibling()) {
                assertQueryFieldIsMetaField(property.getFieldName() == metaField, metaField);
                invariantStatusOK(property.rename(getRenamedField(property.getFieldName())));
            }
        }

        return;
    }

    if (isTopLevelField && (fieldName.empty() || fieldName[0] != '$')) {
        assertQueryFieldIsMetaField(isMetaFieldFirstElementOfDottedPathField(fieldName, metaField),
                                    metaField);
        invariantStatusOK(elem.rename(getRenamedField(fieldName)));
    }

    isTopLevelField = parentIsArray && elem.isType(BSONType::Object);
    parentIsArray = elem.isType(BSONType::Array);
    for (auto child = elem.leftChild(); child.ok(); child = child.rightSibling()) {
        replaceQueryMetaFieldName(child, metaField, isTopLevelField, parentIsArray);
    }
}
}  // namespace

BSONObj translateQuery(const BSONObj& query, StringData metaField) {
    invariant(!metaField.empty());

    mutablebson::Document queryDoc(query);
    for (auto queryElem = queryDoc.root().leftChild(); queryElem.ok();
         queryElem = queryElem.rightSibling()) {
        replaceQueryMetaFieldName(queryElem, metaField);
    }

    return queryDoc.getObject();
}

write_ops::UpdateModification translateUpdate(const write_ops::UpdateModification& updateMod,
                                              StringData metaField) {
    invariant(!metaField.empty());
    invariant(updateMod.type() != write_ops::UpdateModification::Type::kDelta);

    uassert(ErrorCodes::InvalidOptions,
            "Cannot perform an update on a time-series collection using a pipeline update",
            updateMod.type() != write_ops::UpdateModification::Type::kPipeline);

    uassert(ErrorCodes::InvalidOptions,
            "Cannot perform an update on a time-series collection using a replacement document",
            updateMod.type() != write_ops::UpdateModification::Type::kReplacement);

    const auto& document = updateMod.getUpdateModifier();

    // Make a mutable copy of the update document in order to replace all occurrences of the
    // metaField with "meta".
    mutablebson::Document updateDoc{document};
    // updateDoc = { <updateOperator> : { <field1>: <value1>, ... },
    //               <updateOperator> : { <field1>: <value1>, ... },
    //               ... }

    // updateDoc.root() = <updateOperator> : { <field1>: <value1>, ... },
    //                    <updateOperator> : { <field1>: <value1>, ... },
    //                    ...
    for (auto updatePair = updateDoc.root().leftChild(); updatePair.ok();
         updatePair = updatePair.rightSibling()) {
        // updatePair = <updateOperator> : { <field1>: <value1>, ... }
        // Check each field that is being modified by the update operator and replace it if it is
        // the metaField.
        for (auto fieldValuePair = updatePair.leftChild(); fieldValuePair.ok();
             fieldValuePair = fieldValuePair.rightSibling()) {
            auto fieldName = fieldValuePair.getFieldName();

            assertUpdateFieldIsMetaField(
                isMetaFieldFirstElementOfDottedPathField(fieldName, metaField), metaField);
            invariantStatusOK(fieldValuePair.rename(getRenamedField(fieldName)));

            // If this is a $rename, we also need to translate the value.
            if (updatePair.getFieldName() == "$rename") {
                assertUpdateFieldIsMetaField(fieldValuePair.isType(BSONType::String) &&
                                                 isMetaFieldFirstElementOfDottedPathField(
                                                     fieldValuePair.getValueString(), metaField),
                                             metaField);
                invariantStatusOK(fieldValuePair.setValueString(
                    getRenamedField(fieldValuePair.getValueString())));
            }
        }
    }

    return write_ops::UpdateModification::parseFromClassicUpdate(updateDoc.getObject());
}

std::function<size_t(const BSONObj&)> numMeasurementsForBucketCounter(StringData timeField) {
    return [timeField = timeField.toString()](const BSONObj& bucket) {
        return BucketUnpacker::computeMeasurementCount(bucket, timeField);
    };
}

BSONObj getBucketLevelPredicateForRouting(const BSONObj& originalQuery,
                                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          boost::optional<StringData> metaField) {
    if (!metaField) {
        // In case the time-series collection does not have meta field defined, we target the
        // request to all shards using empty predicate. Since we allow only delete requests with
        // 'limit:0', we will not delete any extra documents.
        //
        // TODO SERVER-73087 / SERVER-75160: Move this block into the if
        // gTimeseriesDeletesSupport is not enabled block as soon as we implement either one
        // of SERVER-73087 and SERVER-75160. As of now, this block is common, irrespective of the
        // feature flag value.
        return BSONObj();
    }

    if (!feature_flags::gTimeseriesDeletesSupport.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        // Translate the delete query into a query on the time-series collection's underlying
        // buckets collection.
        return timeseries::translateQuery(originalQuery, *metaField);
    }

    auto swMatchExpr =
        MatchExpressionParser::parse(originalQuery,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    uassertStatusOKWithContext(swMatchExpr.getStatus(), "Failed to parse delete query");
    auto metaFieldStr = metaField->toString();
    // Split out the bucket-level predicate from the delete query and rename the meta field to the
    // internal name, 'meta'.
    auto [bucketLevelPredicate, _] = expression::splitMatchExpressionBy(
        std::move(swMatchExpr.getValue()),
        {metaFieldStr} /*fields*/,
        {{metaFieldStr, timeseries::kBucketMetaFieldName.toString()}} /*renames*/,
        expression::isOnlyDependentOn);

    if (bucketLevelPredicate) {
        return bucketLevelPredicate->serialize();
    }

    // In case that the delete query does not contain bucket-level predicate that can be split out
    // and renamed, target the request to all shards using empty predicate.
    return BSONObj();
}
}  // namespace mongo::timeseries
