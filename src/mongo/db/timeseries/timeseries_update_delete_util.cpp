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
 * Returns a string where the substring leading up to "." in the given field is replaced with
 * newField. If there is no "." in the given field, returns newField.
 */
std::string getRenamedField(StringData field, StringData newField) {
    size_t dotIndex = field.find('.');
    return dotIndex != std::string::npos
        ? newField.toString() + field.substr(dotIndex, field.size() - dotIndex)
        : newField.toString();
}

/**
 * Replaces the first occurrence of the metaField in the given field of the given mutablebson
 * element with the literal "meta", accounting for uses of the metaField with dot notation.
 * shouldReplaceFieldValue is set for $expr queries when "$" + the metaField should be substituted
 * for "$meta".
 */
void replaceQueryMetaFieldName(mutablebson::Element elem,
                               StringData field,
                               StringData metaField,
                               bool shouldReplaceFieldValue = false) {
    if (isMetaFieldFirstElementOfDottedPathField(field, metaField)) {
        invariantStatusOK(elem.rename(getRenamedField(field, "meta")));
    }
    // Substitute element fieldValue with "$meta" if element is a subField of $expr, not a subField
    // of $literal, and the element fieldValue is "$" + the metaField. For example, the following
    // query { q: { $expr: { $gt: [ "$<metaField>" , 100 ] } } } would be translated to
    // { q: { $expr: { $gt: [ "$meta" , 100 ] } } }.
    else if (shouldReplaceFieldValue && elem.isType(BSONType::String) &&
             isMetaFieldFirstElementOfDottedPathField(elem.getValueString(), "$" + metaField)) {
        invariantStatusOK(elem.setValueString(getRenamedField(elem.getValueString(), "$meta")));
    }
}

/**
 * Recurses through the mutablebson element query and replaces any occurrences of the metaField with
 * "meta" accounting for queries that may be in dot notation. shouldReplaceFieldValue is set for
 * $expr queries when "$" + the metaField should be substituted for "$meta".
 */
void replaceQueryMetaFieldName(mutablebson::Element elem,
                               StringData metaField,
                               bool shouldReplaceFieldValue = false) {
    shouldReplaceFieldValue = (elem.getFieldName() != "$literal") &&
        (shouldReplaceFieldValue || (elem.getFieldName() == "$expr"));
    replaceQueryMetaFieldName(elem, elem.getFieldName(), metaField, shouldReplaceFieldValue);
    for (size_t i = 0; i < elem.countChildren(); ++i) {
        replaceQueryMetaFieldName(elem.findNthChild(i), metaField, shouldReplaceFieldValue);
    }
}
}  // namespace

bool queryOnlyDependsOnMetaField(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const BSONObj& query,
                                 boost::optional<StringData> metaField,
                                 const LegacyRuntimeConstants& runtimeConstants,
                                 const boost::optional<BSONObj>& letParams) {
    boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContext(opCtx, nullptr, ns, runtimeConstants, letParams));
    std::vector<BSONObj> rawPipeline{BSON("$match" << query)};
    DepsTracker dependencies = Pipeline::parse(rawPipeline, expCtx)->getDependencies({});
    return metaField
        ? std::all_of(dependencies.fields.begin(),
                      dependencies.fields.end(),
                      [metaField](const auto& dependency) {
                          StringData queryField(dependency);
                          return isMetaFieldFirstElementOfDottedPathField(queryField, *metaField) ||
                              isMetaFieldFirstElementOfDottedPathField(queryField,
                                                                       "$" + *metaField);
                      })
        : dependencies.fields.empty();
}

bool updateOnlyModifiesMetaField(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const mongo::write_ops::UpdateModification& updateMod,
                                 StringData metaField) {
    invariant(updateMod.type() != write_ops::UpdateModification::Type::kDelta);
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Pipeline upddates are not supported for time-series collections: "
                          << ns,
            updateMod.type() != write_ops::UpdateModification::Type::kPipeline);

    const auto& document = updateMod.getUpdateClassic();
    uassert(ErrorCodes::InvalidOptions,
            str::stream()
                << "Replacement document updates are not supported for time-series collections: "
                << ns,
            !isDocReplacement(document));

    return std::all_of(document.begin(), document.end(), [metaField](const auto& updatePair) {
        // updatePair = <updateOperator> : {<field1> : <value1>, ... }
        // updatePair.embeddedObject() = {<field1> : <value1>, ... }
        return std::all_of(updatePair.embeddedObject().begin(),
                           updatePair.embeddedObject().end(),
                           [metaField](const auto& fieldValuePair) {
                               return isMetaFieldFirstElementOfDottedPathField(
                                   fieldValuePair.fieldNameStringData(), metaField);
                           });
    });
}

BSONObj translateQuery(const BSONObj& query, StringData metaField) {
    invariant(!metaField.empty());
    mutablebson::Document queryDoc(query);
    replaceQueryMetaFieldName(queryDoc.root(), metaField);
    return queryDoc.getObject();
}

write_ops::UpdateModification translateUpdate(const write_ops::UpdateModification& updateMod,
                                              StringData metaField) {
    invariant(!metaField.empty());
    invariant(updateMod.type() == write_ops::UpdateModification::Type::kClassic);

    const auto& document = updateMod.getUpdateClassic();
    invariant(!isDocReplacement(document));

    // Make a mutable copy of the update document in order to replace all occurrences of the
    // metaField with "meta".
    auto updateDoc = mutablebson::Document(document);
    // updateDoc = { <updateOperator> : { <field1>: <value1>, ... },
    //               <updateOperator> : { <field1>: <value1>, ... },
    //               ... }

    // updateDoc.root() = <updateOperator> : { <field1>: <value1>, ... },
    //                    <updateOperator> : { <field1>: <value1>, ... },
    //                    ...
    for (size_t i = 0; i < updateDoc.root().countChildren(); ++i) {
        // TODO: SERVER-59104 Remove usages of findNthChild().
        auto updatePair = updateDoc.root().findNthChild(i);
        // updatePair = <updateOperator> : { <field1>: <value1>, ... }
        // Check each field that is being modified by the update operator
        // and replace it if it is the metaField.
        for (size_t j = 0; j < updatePair.countChildren(); j++) {
            // TODO: SERVER-59104 Remove usages of findNthChild().
            auto fieldValuePair = updatePair.findNthChild(j);
            replaceQueryMetaFieldName(fieldValuePair, fieldValuePair.getFieldName(), metaField);
        }
    }

    return write_ops::UpdateModification::parseFromClassicUpdate(updateDoc.getObject());
}
}  // namespace mongo::timeseries
