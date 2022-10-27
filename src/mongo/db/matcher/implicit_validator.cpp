/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/matcher/implicit_validator.h"

#include "mongo/db/query/stage_types.h"
#include <algorithm>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/doc_validation_util.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"

namespace mongo {
namespace {

struct Node {
    std::string name;
    boost::optional<BSONType> type;
    std::vector<Node> subobjs;
};

/*
 * Converts the list of encrypted field paths into a tree where each node
 * (except the root) represents a field path component. Each leaf node
 * contains the expected BSON type of the field value before encryption.
 *
 * ex. [{"a.b.c", "string"}, {"a.x.y", "int"}] =>
 *  {a: {b: {c: {bsonType: 2 }}, x: {y: {bsonType: 16}}}}
 */
std::unique_ptr<Node> buildTreeFromEncryptedFieldPaths(
    const std::vector<EncryptedField>& encryptedFields) {
    auto root = std::make_unique<Node>();
    for (auto& field : encryptedFields) {
        FieldRef fieldPath(field.getPath());
        Node* level = root.get();

        for (FieldIndex i = 0; i < fieldPath.numParts(); i++) {
            auto fieldName = fieldPath.getPart(i);

            auto itr = std::find_if(level->subobjs.begin(),
                                    level->subobjs.end(),
                                    [&fieldName](auto& node) { return fieldName == node.name; });

            if (itr == level->subobjs.end()) {
                // the rest of the path forms a new branch; append nodes until the last part
                for (; i < fieldPath.numParts(); i++) {
                    level->subobjs.push_back(
                        {fieldPath.getPart(i).toString(), boost::none, std::vector<Node>()});
                    level = &(level->subobjs.back());
                }
                if (field.getBsonType().has_value()) {
                    level->type = typeFromName(field.getBsonType().value());
                }
            } else {
                // another field with the same prefix already exists in the tree.
                // make sure that the nodes in common are not leaves
                tassert(6364302,
                        str::stream()
                            << "Encrypted field " << fieldPath.dottedField()
                            << " conflicts with another encrypted field with the same prefix",
                        i < (fieldPath.numParts() - 1) && !itr->subobjs.empty());
                level = &(*itr);
            }
        }
    }
    root->type = BSONType::Object;
    return root;
}

using ErrorAnnotation = MatchExpression::ErrorAnnotation;
using AnnotationMode = ErrorAnnotation::Mode;

std::unique_ptr<MatchExpression> createNotTypeExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    MatcherTypeSet typeSet,
    StringData path,
    bool ignoreError = true) {
    auto annotation = ignoreError
        ? doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore)
        : doc_validation_error::createAnnotation(
              expCtx, "type", BSON("type" << typeSet.toBSONArray()));
    auto typeExpr =
        std::make_unique<InternalSchemaTypeExpression>(path, typeSet, std::move(annotation));

    annotation = ignoreError
        ? doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore)
        : doc_validation_error::createAnnotation(expCtx, "$not", BSONObj());
    return std::make_unique<NotMatchExpression>(std::move(typeExpr), std::move(annotation));
}

std::unique_ptr<MatchExpression> createObjectExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData path,
    std::unique_ptr<MatchExpression> subschema) {
    auto objectMatch = std::make_unique<InternalSchemaObjectMatchExpression>(
        path,
        std::move(subschema),
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));
    auto orExpr = std::make_unique<OrMatchExpression>(
        doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));
    orExpr->add(createNotTypeExpression(expCtx, BSONType::Object, path));
    orExpr->add(std::move(objectMatch));
    return orExpr;
}

std::unique_ptr<MatchExpression> treeToMatchExpression(
    const Node& node, const boost::intrusive_ptr<ExpressionContext>& expCtx, bool toplevel) {

    if (node.subobjs.empty()) {
        // leaf node expression:
        // { $and : [
        //     { <node.name> : { $_internalSchemaBinDataFLE2EncryptedType : <node.type> } },
        // ]}
        auto andExpr = std::make_unique<AndMatchExpression>(doc_validation_error::createAnnotation(
            expCtx, "_property", BSON("propertyName" << node.name)));

        andExpr->add(std::make_unique<InternalSchemaBinDataFLE2EncryptedTypeExpression>(
            StringData(node.name),
            node.type.has_value() ? MatcherTypeSet(node.type.value()) : MatcherTypeSet(),
            doc_validation_error::createAnnotation(expCtx, "fle2Encrypt", BSONObj())));

        return andExpr;
    }

    // { $and : [
    //     { $or : [
    //         { <subnode.name> : { $not : { $exists: true } } },
    //         <recursion_result(subnode)>,
    //     ]},
    //     ... /* for each subnode */
    // ]}
    auto subschema = std::make_unique<AndMatchExpression>(
        doc_validation_error::createAnnotation(expCtx, "properties", BSONObj()));
    for (auto& subnode : node.subobjs) {
        auto existsExpr = std::make_unique<ExistsMatchExpression>(
            StringData(subnode.name),
            doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));
        auto notExpr = std::make_unique<NotMatchExpression>(
            std::move(existsExpr),
            doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));
        auto orExpr = std::make_unique<OrMatchExpression>(
            doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnoreButDescend));

        orExpr->add(std::move(notExpr));
        orExpr->add(treeToMatchExpression(subnode, expCtx, false));
        subschema->add(std::move(orExpr));
    }

    // if top-level, wrap the "and" expression with an outer "and" annotated with $and.
    if (toplevel) {
        auto outerAndExpr = std::make_unique<AndMatchExpression>(
            doc_validation_error::createAnnotation(expCtx, "implicitFLESchema", BSONObj()));
        outerAndExpr->add(std::move(subschema));
        return outerAndExpr;
    }

    // otherwise, create the following and expression:
    // { $and : [
    //     { $or : [
    //         { <node.name> : { $not : { $_internalSchemaType: "object" } } },
    //         { <node.name> : { $_internalSchemaObjectMatch : <subschema> } },
    //     ]},
    //     { <node.name> : { $not : { $_internalSchemaType: "array" } } },
    // ]}
    // where <subschema> = $and from above loop
    // The first clause fails to match if the node is an object that does not satisfy the
    // subschema. The second disallows arrays along the encrypted field path.
    auto andExpr = std::make_unique<AndMatchExpression>(doc_validation_error::createAnnotation(
        expCtx, "_property", BSON("propertyName" << node.name)));
    andExpr->add(createObjectExpression(expCtx, node.name, std::move(subschema)));
    andExpr->add(
        createNotTypeExpression(expCtx, BSONType::Array, node.name, false /* ignoreError */));
    return andExpr;
}

}  // namespace

StatusWithMatchExpression generateMatchExpressionFromEncryptedFields(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::vector<EncryptedField>& encryptedFields) try {

    if (encryptedFields.empty()) {
        return std::make_unique<AlwaysTrueMatchExpression>(
            doc_validation_error::createAnnotation(expCtx, AnnotationMode::kIgnore));
    }

    auto root = buildTreeFromEncryptedFieldPaths(encryptedFields);
    return treeToMatchExpression(*root, expCtx, true /* toplevel */);
} catch (...) {
    return exceptionToStatus();
}

}  // namespace mongo
