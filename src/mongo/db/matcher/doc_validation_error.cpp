/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/doc_validation_error.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/match_expression_walker.h"

namespace mongo::doc_validation_error {
namespace {
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(DocumentValidationFailureInfo);

/**
 * Enumerated type which describes whether an error should be described normally or in an
 * inverted sense when in a negated context. More precisely, when a MatchExpression fails to match a
 * document, the generated error will refer to failure unless the MatchExpression is nested
 * within another MatchExpression that expresses a logical negation, in which case the generated
 * error will refer to success.
 */
enum class InvertError { kNormal, kInverted };

/**
 * A struct which tracks context during error generation.
 */
struct ValidationErrorContext {
    ValidationErrorContext(const MatchableDocument* doc) : doc(doc) {}

    /**
     * Returns the complete document validation error as a BSONObj once error generation has
     * finished.
     */
    BSONObj done() {
        // When error generation is finished, there must be exactly one BSONObjBuilder which
        // contains the complete error.
        return objBuilder.obj();
    }

    BSONObjBuilder& getCurrentObjBuilder() {
        return objBuilder;
    }

    /**
     * Sets 'inversion' to the opposite of its current value.
     */
    void flipInversion() {
        inversion =
            inversion == InvertError::kNormal ? InvertError::kInverted : InvertError::kNormal;
    }

    // BSONObjBuilder which is used to construct the generated error.
    BSONObjBuilder objBuilder;
    // Document which failed to match against the collection's validator.
    const MatchableDocument* doc;
    // Tracks whether the generated error should be described normally or in an inverted context.
    InvertError inversion = InvertError::kNormal;
};

using ErrorAnnotation = MatchExpression::ErrorAnnotation;
using Mode = ErrorAnnotation::Mode;

/**
 * Visitor which is primarily responsible for error generation.
 */
class ValidationErrorPreVisitor final : public MatchExpressionConstVisitor {
public:
    ValidationErrorPreVisitor(ValidationErrorContext* context) : _context(context) {}
    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {}
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}
    void visit(const EqualityMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const InMatchExpression* expr) final {
        static constexpr auto kNormalReason = "no matching value found in array";
        static constexpr auto kInvertedReason = "matching value found in array";
        generateLeafError(expr, kNormalReason, kInvertedReason);
    }
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {
        _context->flipInversion();
    }
    void visit(const OrMatchExpression* expr) final {}
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }

private:
    // Set of utilities responsible for appending various fields to build a descriptive error.
    void appendOperatorName(const ErrorAnnotation& annotation, BSONObjBuilder* bob) {
        bob->append("operatorName", annotation.operatorName);
    }
    void appendSpecifiedAs(const ErrorAnnotation& annotation, BSONObjBuilder* bob) {
        bob->append("specifiedAs", annotation.annotation);
    }

    /**
     * Given a pointer to a LeafMatchExpression 'expr', appends details to the current
     * BSONObjBuilder tracked by '_context' describing why the document failed to match against
     * 'expr'. In particular:
     * - Appends "reason: field was missing" if expr's path is missing from the document.
     * - Appends the specified 'reason' along with 'consideredValue' if the 'path' in the
     * document resolves to a single value.
     * - Appends the specified 'reason' along with 'consideredValues' if the 'path' in the
     * document resolves to an array of values that is implicitly traversed by 'expr'.
     */
    void appendLeafErrorDetails(const LeafMatchExpression* expr, const std::string& reason) {
        BSONObjBuilder* bob = &(_context->getCurrentObjBuilder());
        ElementPath path(expr->path());
        MatchableDocument::IteratorHolder cursor(_context->doc, &path);
        BSONArrayBuilder bab;
        while (cursor->more()) {
            auto elem = cursor->next().element();
            if (elem.eoo()) {
                break;
            } else {
                bab.append(elem);
            }
        }
        auto size = bab.arrSize();
        if (size == 0) {
            bob->append("reason", "field was missing");
        } else {
            bob->append("reason", reason);
            auto arr = bab.arr();
            if (size == 1) {
                bob->appendAs(arr[0], "consideredValue");
            } else {
                bob->append("consideredValues", arr);
            }
        }
    }

    void generateLeafError(const LeafMatchExpression* expr,
                           const std::string& normalReason,
                           const std::string& invertedReason) {
        if (auto annotationPtr = expr->getErrorAnnotation()) {
            const auto& annotation = *annotationPtr;
            BSONObjBuilder& bob = _context->getCurrentObjBuilder();
            if (annotation.mode == Mode::kGenerateError) {
                appendOperatorName(annotation, &bob);
                appendSpecifiedAs(annotation, &bob);
                if (_context->inversion == InvertError::kNormal) {
                    appendLeafErrorDetails(expr, normalReason);
                } else {
                    appendLeafErrorDetails(expr, invertedReason);
                }
            }
        }
    }

    void generateComparisonError(const ComparisonMatchExpression* expr) {
        static constexpr auto normalReason = "comparison failed";
        static constexpr auto invertedReason = "comparison succeeded";
        generateLeafError(expr, normalReason, invertedReason);
    }

    ValidationErrorContext* _context;
};

/**
 * Visitor which maintains state for tree MatchExpressions in between visiting each child.
 */
class ValidationErrorInVisitor final : public MatchExpressionConstVisitor {
public:
    ValidationErrorInVisitor(ValidationErrorContext* context) : _context(context) {}
    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {}
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {}
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }

private:
    ValidationErrorContext* _context;
};

/**
 * Visitor which finalizes the generated error for the current MatchExpression.
 */
class ValidationErrorPostVisitor final : public MatchExpressionConstVisitor {
public:
    ValidationErrorPostVisitor(ValidationErrorContext* context) : _context(context) {}
    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {}
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {
        _context->flipInversion();
    }
    void visit(const OrMatchExpression* expr) final {}
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }

private:
    ValidationErrorContext* _context;
};

}  // namespace

std::shared_ptr<const ErrorExtraInfo> DocumentValidationFailureInfo::parse(const BSONObj& obj) {
    auto errInfo = obj["errInfo"];
    uassert(4878100,
            "DocumentValidationFailureInfo must have a field 'errInfo' of type object",
            errInfo.type() == BSONType::Object);
    return std::make_shared<DocumentValidationFailureInfo>(errInfo.embeddedObject());
}

void DocumentValidationFailureInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("errInfo", _details);
}
const BSONObj& DocumentValidationFailureInfo::getDetails() const {
    return _details;
}
BSONObj generateError(const MatchExpression& validatorExpr, const BSONObj& doc) {
    BSONMatchableDocument matchableDoc(doc);
    ValidationErrorContext context(&matchableDoc);
    ValidationErrorPreVisitor preVisitor{&context};
    ValidationErrorInVisitor inVisitor{&context};
    ValidationErrorPostVisitor postVisitor{&context};
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(&validatorExpr, &walker);
    return context.done();
}

}  // namespace mongo::doc_validation_error