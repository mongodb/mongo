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

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/timeseries/timeseries_constants.h"

namespace mongo::timeseries {
namespace {
/**
 * Returns whether the given document is a replacement document.
 */
bool isDocReplacement(const BSONObj& doc) {
    return doc.isEmpty() || (doc.firstElementFieldNameStringData().find("$") == std::string::npos);
}

/**
 * Returns whether the given metaField is the first element of the dotted path in the given
 * field.
 */
bool isMetaFieldFirstElementOfDottedPathField(StringData field, StringData metaField) {
    return field.substr(0, field.find('.')) == metaField;
}

/**
 * Returns a string where the substring leading up to "." in the given field is replaced with the
 * literal "meta". If there is no "." in the given field, returns "meta".
 */
std::string getRenamedMetaField(StringData field) {
    size_t dotIndex = field.find('.');
    return dotIndex != std::string::npos ? "meta" + field.substr(dotIndex, field.size() - dotIndex)
                                         : "meta";
}

/**
 * Replaces the first occurrence of the metaField in the given field of the given mutablebson
 * element with the literal "meta", accounting for uses of the metaField with dot notation.
 */
void replaceQueryMetaFieldName(mutablebson::Element elem, StringData field, StringData metaField) {
    if (isMetaFieldFirstElementOfDottedPathField(field, metaField)) {
        invariantStatusOK(elem.rename(getRenamedMetaField(field)));
    }
}

/**
 * Recurses through the mutablebson element query and replaces any occurrences of the
 * metaField with "meta" accounting for queries that may be in dot notation. shouldReplaceFieldValue
 * is set for $expr queries when "$" + the metaField should be substituted for "$meta".
 */
void replaceQueryMetaFieldName(mutablebson::Element elem, StringData metaField) {
    replaceQueryMetaFieldName(elem, elem.getFieldName(), metaField);
    for (size_t i = 0; i < elem.countChildren(); ++i) {
        replaceQueryMetaFieldName(elem.findNthChild(i), metaField);
    }
}
}  // namespace

// TODO: SERVER-58382 Handle time-series collections without a metaField.
bool queryOnlyDependsOnMetaField(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const BSONObj& query,
                                 StringData metaField) {
    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, nullptr, ns));
    std::vector<BSONObj> rawPipeline{BSON("$match" << query)};
    DepsTracker dependencies = Pipeline::parse(rawPipeline, expCtx)->getDependencies({});
    return std::all_of(dependencies.fields.begin(),
                       dependencies.fields.end(),
                       [metaField](const auto& dependency) {
                           return isMetaFieldFirstElementOfDottedPathField(StringData(dependency),
                                                                           metaField);
                       });
}

bool updateOnlyModifiesMetaField(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const mongo::write_ops::UpdateModification& updateMod,
                                 StringData metaField) {
    switch (updateMod.type()) {
        case write_ops::UpdateModification::Type::kClassic: {
            const auto& document = updateMod.getUpdateClassic();
            if (isDocReplacement(document))
                return false;
            // else document is an update document.
            for (auto&& updatePair : document) {
                // updatePair = <updateOperator> : {<field1> : <value1>, ... }
                for (auto&& fieldValuePair : updatePair.embeddedObject()) {
                    // updatePair.embeddedObject() = {<field1> : <value1>, ... }
                    if (!isMetaFieldFirstElementOfDottedPathField(
                            fieldValuePair.fieldNameStringData(), metaField))
                        return false;
                }
            }
            break;
        }
        case write_ops::UpdateModification::Type::kPipeline: {
            const auto& updatePipeline = updateMod.getUpdatePipeline();
            for (const auto& stage : updatePipeline) {
                auto aggOp = stage.firstElementFieldNameStringData();
                auto operation = stage.firstElement();
                if (aggOp == "$set" || aggOp == "$addFields") {
                    // stage = {$set: {<newField> : <newExpression>, ...}}
                    // operation = $set: {<newField> : <newExpression>, ...}
                    for (auto&& updatePair : operation.embeddedObject()) {
                        // operation.embeddedObject() = {<newField> : <newExpression>, ...}
                        if (!isMetaFieldFirstElementOfDottedPathField(
                                updatePair.fieldNameStringData(), metaField)) {
                            return false;
                        }
                    }
                } else if (aggOp == "$unset" || aggOp == "$project") {
                    if (operation.type() == BSONType::Array) {
                        // stage = {$unset: ["field1", "field2", ...]}
                        for (auto elt : operation.Array()) {
                            if (!isMetaFieldFirstElementOfDottedPathField(elt.valueStringDataSafe(),
                                                                          metaField))
                                return false;
                        }
                    } else {
                        // stage = {$unset: "singleField"}
                        if (!isMetaFieldFirstElementOfDottedPathField(
                                operation.valueStringDataSafe(), metaField))
                            return false;
                    }
                } else {  // aggOp == "$replaceWith" || aggOp == "$replaceRoot"
                    return false;
                }
            }
            break;
        }
        case write_ops::UpdateModification::Type::kDelta:
            // It is not possible for the client to run a delta update.
            MONGO_UNREACHABLE;
    }
    return true;
}

BSONObj translateQuery(const BSONObj& query, StringData metaField) {
    invariant(!metaField.empty());
    mutablebson::Document queryDoc(query);
    timeseries::replaceQueryMetaFieldName(queryDoc.root(), metaField);
    return queryDoc.getObject();
}

write_ops::UpdateOpEntry translateUpdate(const BSONObj& translatedQuery,
                                         const write_ops::UpdateModification& updateMod,
                                         StringData metaField) {
    // Make a mutable copy of the updates to apply where we can replace all occurrences
    // of the metaField with "meta". The update is either a document or a pipeline.
    switch (updateMod.type()) {
        case write_ops::UpdateModification::Type::kClassic: {
            const auto& document = updateMod.getUpdateClassic();
            if (isDocReplacement(document))
                break;

            // Make a mutable copy of the update document.
            auto updateDoc = mutablebson::Document(document);
            // updateDoc = { <updateOperator> : { <field1>: <value1>, ... },
            //               <updateOperator> : { <field1>: <value1>, ... },
            //               ... }

            // updateDoc.root() = <updateOperator> : { <field1>: <value1>, ... },
            //                    <updateOperator> : { <field1>: <value1>, ... },
            //                    ...
            for (size_t i = 0; i < updateDoc.root().countChildren(); ++i) {
                auto updatePair = updateDoc.root().findNthChild(i);
                // updatePair = <updateOperator> : { <field1>: <value1>, ... }
                // Check each field that is being modified by the update operator
                // and replace it if it is the metaField.
                for (size_t j = 0; j < updatePair.countChildren(); j++) {
                    auto fieldValuePair = updatePair.findNthChild(j);
                    timeseries::replaceQueryMetaFieldName(
                        fieldValuePair, fieldValuePair.getFieldName(), metaField);
                }
            }

            // Create a new UpdateOpEntry and add it to the list of translated
            // updates to perform.
            write_ops::UpdateOpEntry newOpEntry(
                translatedQuery,
                write_ops::UpdateModification::parseFromClassicUpdate(updateDoc.getObject()));
            newOpEntry.setMulti(true);
            return newOpEntry;
        }
        case write_ops::UpdateModification::Type::kPipeline: {
            std::vector<BSONObj> translatedPipeline;
            for (const auto& stage : updateMod.getUpdatePipeline()) {
                // stage: {  <$operator> : <argument(s)> }
                mutablebson::Document stageDoc(stage);
                auto root = stageDoc.root();
                for (size_t i = 0; i < root.countChildren(); ++i) {
                    auto updatePair = root.findNthChild(i);
                    auto aggOp = updatePair.getFieldName();
                    if (aggOp == "$set" || aggOp == "$addFields") {
                        // updatePair = $set: {<newField> : <newExpression>, ...}
                        for (size_t j = 0; j < updatePair.countChildren(); j++) {
                            auto fieldValuePair = updatePair.findNthChild(j);
                            timeseries::replaceQueryMetaFieldName(
                                fieldValuePair, fieldValuePair.getFieldName(), metaField);
                        }
                    } else if (aggOp == "$unset" || aggOp == "$project") {
                        if (updatePair.getType() == BSONType::Array) {
                            // updatePair = $unset: ["field1", "field2", ...]
                            for (size_t j = 0; j < updatePair.countChildren(); j++) {
                                timeseries::replaceQueryMetaFieldName(
                                    updatePair,
                                    updatePair.findNthChild(j).getValueString(),
                                    metaField);
                            }
                        } else {  // updatePair.getType() == BSONType::String
                            // updatePair = $unset: "singleField"
                            auto singleField = StringData(updatePair.getValueString());
                            if (isMetaFieldFirstElementOfDottedPathField(singleField, metaField)) {
                                // Replace updatePair with a new pair where singleField is renamed.
                                auto newPair = stageDoc.makeElementString(
                                    aggOp, timeseries::getRenamedMetaField(singleField));
                                updatePair.remove().ignore();
                                root.pushBack(newPair).ignore();
                            }
                        }  // else aggOp == "$replaceWith" || aggOp == "$replaceRoot"
                    }
                }
                // Add the translated pipeline.
                translatedPipeline.push_back(stageDoc.getObject());
            }
            write_ops::UpdateOpEntry newOpEntry(translatedQuery,
                                                write_ops::UpdateModification(translatedPipeline));
            newOpEntry.setMulti(true);
            return newOpEntry;
        }
        case write_ops::UpdateModification::Type::kDelta:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

void replaceTimeseriesQueryMetaFieldName(mutablebson::Element elem,
                                         const StringData& metaField,
                                         bool shouldReplaceFieldValue) {
    invariant(!metaField.empty());
    shouldReplaceFieldValue = (elem.getFieldName() != "$literal") &&
        (shouldReplaceFieldValue || (elem.getFieldName() == "$expr"));
    if (isMetaFieldFirstElementOfDottedPathField(elem.getFieldName(), metaField)) {
        size_t dotIndex = elem.getFieldName().find('.');
        dotIndex >= elem.getFieldName().size()
            ? invariantStatusOK(elem.rename("meta"))
            : invariantStatusOK(elem.rename(
                  "meta" +
                  elem.getFieldName().substr(dotIndex, elem.getFieldName().size() - dotIndex)));
    }
    // Substitute element fieldValue with "$meta" if element is a subField of $expr, not a subField
    // of $literal, and the element fieldValue is "$" + the metaField.
    else if (shouldReplaceFieldValue && elem.isType(BSONType::String) &&
             isMetaFieldFirstElementOfDottedPathField(elem.getValueString(), "$" + metaField)) {
        size_t dotIndex = elem.getValueString().find('.');
        dotIndex >= elem.getValueString().size()
            ? invariantStatusOK(elem.setValueString("$meta"))
            : invariantStatusOK(elem.setValueString(
                  "$meta" +
                  elem.getValueString().substr(dotIndex, elem.getValueString().size() - dotIndex)));
    }
    if (elem.hasChildren()) {
        for (size_t i = 0; i < elem.countChildren(); ++i) {
            replaceTimeseriesQueryMetaFieldName(
                elem.findNthChild(i), metaField, shouldReplaceFieldValue);
        }
    }
}
}  // namespace mongo::timeseries
