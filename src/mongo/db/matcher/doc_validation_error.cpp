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

#include "mongo/db/matcher/doc_validation_error.h"

#include <stack>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"

namespace mongo::doc_validation_error {
namespace {
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(DocumentValidationFailureInfo);

using ErrorAnnotation = MatchExpression::ErrorAnnotation;
using AnnotationMode = ErrorAnnotation::Mode;
using LeafArrayBehavior = ElementPath::LeafArrayBehavior;

/**
 * Enumerated type which describes whether an error should be described normally or in an
 * inverted sense when in a negated context. More precisely, when a MatchExpression fails to match a
 * document, the generated error will refer to failure unless the MatchExpression is nested
 * within another MatchExpression that expresses a logical negation, in which case the generated
 * error will refer to success.
 */
enum class InvertError { kNormal, kInverted };

/**
 * A struct which tracks error generation information for some node within the tree.
 */
struct ValidationErrorFrame {
    /**
     * Enumerated type which describes runtime information about a node participating in error
     * generation.
     */
    enum class RuntimeState {
        // This node contributes to error generation.
        kError,
        // Neither this node nor do any of its children contribute to error generation at all.
        kNoError,
        // This node contributes to error generation, but it needs more information about its child
        // nodes when generating an error. For instance, when generating an error for an AND in a
        // normal context, we need to discern which of its clauses failed.
        kErrorNeedChildrenInfo,
    };

    ValidationErrorFrame(RuntimeState runtimeState, BSONObj currentDoc)
        : runtimeState(runtimeState), currentDoc(std::move(currentDoc)) {}

    // BSONBuilders which construct the generated error.
    BSONObjBuilder objBuilder;
    BSONArrayBuilder arrayBuilder;
    // Tracks the index of the current child expression.
    size_t childIndex = 0;
    // Tracks runtime information about how the current node should generate an error.
    RuntimeState runtimeState;
    // Tracks the current subdocument that an error should be generated over.
    BSONObj currentDoc;
};

using RuntimeState = ValidationErrorFrame::RuntimeState;

/**
 * A struct which tracks context during error generation.
 */
struct ValidationErrorContext {
    ValidationErrorContext(const BSONObj& rootDoc) : rootDoc(rootDoc) {}

    /**
     * Utilities which add/remove ValidationErrorFrames from 'frames'.
     */
    void pushNewFrame(const MatchExpression& expr, const BSONObj& subDoc) {
        // Clear the last error that was generated.
        latestCompleteError = BSONObj();

        // If this is the first frame, then we know that we've failed validation, so we must be
        // generating an error.
        if (frames.empty()) {
            frames.emplace(RuntimeState::kError, subDoc);
            return;
        }

        auto parentRuntimeState = getCurrentRuntimeState();

        // If we've determined at runtime or at parse time that this node shouldn't contribute to
        // error generation, then push a frame indicating that this node should not produce an
        // error and return.
        if (parentRuntimeState == RuntimeState::kNoError ||
            expr.getErrorAnnotation()->mode == AnnotationMode::kIgnore) {
            frames.emplace(RuntimeState::kNoError, subDoc);
            return;
        }
        // If our parent needs more information, call 'matches()' to determine whether we are
        // contributing to error output.
        if (parentRuntimeState == RuntimeState::kErrorNeedChildrenInfo) {
            bool generateErrorValue = expr.matchesBSON(subDoc) ? inversion == InvertError::kInverted
                                                               : inversion == InvertError::kNormal;
            frames.emplace(generateErrorValue ? RuntimeState::kError : RuntimeState::kNoError,
                           subDoc);
            return;
        }
        frames.emplace(RuntimeState::kError, subDoc);
    }
    void popFrame() {
        invariant(!frames.empty());
        frames.pop();
    }

    /**
     * Utilities which return members of the current ValidationContextFrame.
     */
    BSONObjBuilder& getCurrentObjBuilder() {
        invariant(!frames.empty());
        return frames.top().objBuilder;
    }
    BSONArrayBuilder& getCurrentArrayBuilder() {
        invariant(!frames.empty());
        return frames.top().arrayBuilder;
    }
    size_t getCurrentChildIndex() const {
        invariant(!frames.empty());
        return frames.top().childIndex;
    }
    void incrementCurrentChildIndex() {
        invariant(!frames.empty());
        ++frames.top().childIndex;
    }
    RuntimeState getCurrentRuntimeState() const {
        invariant(!frames.empty());
        return frames.top().runtimeState;
    }
    void setCurrentRuntimeState(RuntimeState runtimeState) {
        invariant(!frames.empty());

        // If a node has RuntimeState::kNoError, then its runtime state value should never be
        // modified since the node should never contribute to error generation.
        if (getCurrentRuntimeState() != RuntimeState::kNoError) {
            frames.top().runtimeState = runtimeState;
        }
    }
    const BSONObj& getCurrentDocument() {
        if (!frames.empty()) {
            return frames.top().currentDoc;
        }
        return rootDoc;
    }
    BSONObj getLatestCompleteError() const {
        return latestCompleteError;
    }

    /**
     * Finishes error for 'expr' by stashing its generated error if it made one and popping the
     * frame that it created.
     */
    void finishCurrentError(const MatchExpression* expr) {
        if (shouldGenerateError(*expr)) {
            latestCompleteError = getCurrentObjBuilder().obj();
        }
        popFrame();
    }

    /**
     * Sets 'inversion' to the opposite of its current value.
     */
    void flipInversion() {
        inversion =
            inversion == InvertError::kNormal ? InvertError::kInverted : InvertError::kNormal;
    }

    /**
     * Returns whether 'expr' should generate an error.
     */
    bool shouldGenerateError(const MatchExpression& expr) {
        return expr.getErrorAnnotation()->mode == AnnotationMode::kGenerateError &&
            getCurrentRuntimeState() != RuntimeState::kNoError;
    }

    // Frames which construct the generated error. Each frame corresponds to the information needed
    // to generate an error for one node. As such, each node must call 'pushNewFrame' as part of
    // its pre-visit and 'popFrame' as part of its post-visit.
    std::stack<ValidationErrorFrame> frames;
    // Tracks the most recently completed error. The final error will be stored here.
    BSONObj latestCompleteError;
    // Document which failed to match against the collection's validator.
    const BSONObj& rootDoc;
    // Tracks whether the generated error should be described normally or in an inverted context.
    InvertError inversion = InvertError::kNormal;
};

/**
 * Append the error generated by one of 'expr's children to the current array builder of 'expr'
 * if said child generated an error.
 */
void finishLogicalOperatorChildError(const ListOfMatchExpression* expr,
                                     ValidationErrorContext* ctx) {
    BSONObj childError = ctx->latestCompleteError;
    if (!childError.isEmpty() && ctx->shouldGenerateError(*expr)) {
        auto operatorName = expr->getErrorAnnotation()->operatorName;

        // Only provide the indexes of non-matching clauses for explicit $and/$or/$nor in the
        // user's query.
        if (operatorName == "$and" || operatorName == "$or" || operatorName == "$nor") {
            BSONObjBuilder subBuilder = ctx->getCurrentArrayBuilder().subobjStart();
            subBuilder.appendNumber("index", ctx->getCurrentChildIndex());
            subBuilder.append("details", childError);
            subBuilder.done();
        } else {
            ctx->getCurrentArrayBuilder().append(childError);
        }
    }
    ctx->incrementCurrentChildIndex();
}

/**
 * Visitor which is primarily responsible for error generation.
 */
class ValidationErrorPreVisitor final : public MatchExpressionConstVisitor {
public:
    ValidationErrorPreVisitor(ValidationErrorContext* context) : _context(context) {}
    void visit(const AlwaysFalseMatchExpression* expr) final {
        generateAlwaysBooleanError(*expr);
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        generateAlwaysBooleanError(*expr);
    }
    void visit(const AndMatchExpression* expr) final {
        // $all is treated as a leaf operator.
        if (expr->getErrorAnnotation()->operatorName == "$all") {
            processAll(*expr);
        } else {
            preVisitTreeOperator(expr);
            // An AND needs its children to call 'matches' in a normal context to discern which
            // clauses failed.
            if (_context->inversion == InvertError::kNormal) {
                _context->setCurrentRuntimeState(RuntimeState::kErrorNeedChildrenInfo);
            }
        }
    }
    void visit(const BitsAllClearMatchExpression* expr) final {
        generateError(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        generateError(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        generateError(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        generateError(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        generateElemMatchError(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        generateElemMatchError(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const ExistsMatchExpression* expr) final {
        static constexpr auto normalReason = "path does not exist";
        static constexpr auto invertedReason = "path does exist";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            appendErrorDetails(*expr);
            appendErrorReason(*expr, normalReason, invertedReason);
        }
    }
    void visit(const ExprMatchExpression* expr) final {
        static constexpr auto normalReason = "$expr did not match";
        static constexpr auto invertedReason = "$expr did match";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            appendErrorDetails(*expr);
            appendErrorReason(*expr, normalReason, invertedReason);
            BSONObjBuilder& bob = _context->getCurrentObjBuilder();
            // Append the result of $expr's aggregate expression. The result of the
            // aggregate expression can be determined from the current inversion.
            bob.append("expressionResult", _context->inversion == InvertError::kInverted);
        }
    }
    void visit(const GTEMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const GeoMatchExpression* expr) final {
        static const std::set<BSONType> kExpectedTypes{BSONType::Array, BSONType::Object};
        switch (expr->getGeoExpression().getPred()) {
            case GeoExpression::Predicate::WITHIN: {
                static constexpr auto kNormalReason =
                    "none of considered geometries was contained within the expression’s geometry";
                static constexpr auto kInvertedReason =
                    "at least one of considered geometries was contained within the expression’s "
                    "geometry";
                generatePathError(*expr, kNormalReason, kInvertedReason, &kExpectedTypes);
            } break;
            case GeoExpression::Predicate::INTERSECT: {
                static constexpr auto kNormalReason =
                    "none of considered geometries intersected the expression’s geometry";
                static constexpr auto kInvertedReason =
                    "at least one of considered geometries intersected the expression’s geometry";
                generatePathError(*expr, kNormalReason, kInvertedReason, &kExpectedTypes);
            } break;
            default:
                MONGO_UNREACHABLE;
        }
    }
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const InMatchExpression* expr) final {
        static constexpr auto kNormalReason = "no matching value found in array";
        static constexpr auto kInvertedReason = "matching value found in array";
        generatePathError(*expr, kNormalReason, kInvertedReason);
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
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        // This node should never be responsible for generating an error directly.
        invariant(expr->getErrorAnnotation()->mode != AnnotationMode::kGenerateError);
        BSONObj subDocument = _context->getCurrentDocument();
        ElementPath path(expr->path(), LeafArrayBehavior::kNoTraversal);
        BSONMatchableDocument doc(_context->getCurrentDocument());
        MatchableDocument::IteratorHolder cursor(&doc, &path);
        invariant(cursor->more());
        auto elem = cursor->next().element();

        // If we do not find an object at expr's path, then the subtree rooted at this node will
        // not contribute to error generation as there will either be an explicit
        // ExistsMatchExpression which will explain a missing path error or an explicit
        // InternalSchemaTypeExpression that will explain a type did not match error.
        bool ignoreSubTree = false;
        if (elem.type() == BSONType::Object) {
            subDocument = elem.embeddedObject();
        } else {
            ignoreSubTree = true;
        }

        // This expression should match exactly one object; if there are any more elements, then
        // ignore the subtree.
        if (cursor->more()) {
            ignoreSubTree = true;
        }
        _context->pushNewFrame(*expr, subDocument);
        if (ignoreSubTree) {
            _context->setCurrentRuntimeState(RuntimeState::kNoError);
        }
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {
        generateTypeError(expr, LeafArrayBehavior::kNoTraversal);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const ModMatchExpression* expr) final {
        static constexpr auto kNormalReason = "$mod did not evaluate to expected remainder";
        static constexpr auto kInvertedReason = "$mod did evaluate to expected remainder";
        static const std::set<BSONType> expectedTypes{
            NumberLong, NumberDouble, NumberDecimal, NumberInt};
        generatePathError(*expr, kNormalReason, kInvertedReason, &expectedTypes);
    }
    void visit(const NorMatchExpression* expr) final {
        preVisitTreeOperator(expr);
        // A NOR needs its children to call 'matches' in a normal context to discern which
        // clauses matched.
        if (_context->inversion == InvertError::kNormal) {
            _context->setCurrentRuntimeState(RuntimeState::kErrorNeedChildrenInfo);
        }
        _context->flipInversion();
    }
    void visit(const NotMatchExpression* expr) final {
        preVisitTreeOperator(expr);
        _context->flipInversion();
    }
    void visit(const OrMatchExpression* expr) final {
        preVisitTreeOperator(expr);
        // An OR needs its children to call 'matches' in an inverted context to discern which
        // clauses matched.
        if (_context->inversion == InvertError::kInverted) {
            _context->setCurrentRuntimeState(RuntimeState::kErrorNeedChildrenInfo);
        }
    }
    void visit(const RegexMatchExpression* expr) final {
        static constexpr auto kNormalReason = "regular expression did not match";
        static constexpr auto kInvertedReason = "regular expression did match";
        static const std::set<BSONType> expectedTypes{String, Symbol, RegEx};
        generatePathError(*expr, kNormalReason, kInvertedReason, &expectedTypes);
    }
    void visit(const SizeMatchExpression* expr) final {
        static constexpr auto kNormalReason = "array length was not equal to given size";
        static constexpr auto kInvertedReason = "array length was equal to given size";
        generateArrayError(expr, kNormalReason, kInvertedReason);
    }
    void visit(const TextMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {
        generateTypeError(expr, LeafArrayBehavior::kTraverse);
    }
    void visit(const WhereMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }

private:
    // Set of utilities responsible for appending various fields to build a descriptive error.
    void appendOperatorName(const ErrorAnnotation& annotation, BSONObjBuilder* bob) {
        auto operatorName = annotation.operatorName;
        // Only append the operator name if 'annotation' has one.
        if (!operatorName.empty()) {
            bob->append("operatorName", operatorName);
        }
    }
    void appendSpecifiedAs(const ErrorAnnotation& annotation, BSONObjBuilder* bob) {
        bob->append("specifiedAs", annotation.annotation);
    }
    void appendErrorDetails(const MatchExpression& expr) {
        auto annotation = expr.getErrorAnnotation();
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        appendOperatorName(*annotation, &bob);
        appendSpecifiedAs(*annotation, &bob);
    }

    BSONArray createValuesArray(const ElementPath& path) {
        BSONMatchableDocument doc(_context->getCurrentDocument());
        MatchableDocument::IteratorHolder cursor(&doc, &path);
        BSONArrayBuilder bab;
        while (cursor->more()) {
            auto elem = cursor->next().element();
            if (elem.eoo()) {
                break;
            } else {
                bab.append(elem);
            }
        }
        return bab.arr();
    }
    /**
     * Appends a missing field error if 'arr' is empty.
     */
    void appendMissingField(const BSONArray& arr) {
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        if (arr.isEmpty()) {
            bob.append("reason", "field was missing");
        }
    }

    /**
     * Appends a type mismatch error if no elements in 'arr' have one of the expected types.
     */
    void appendTypeMismatch(const BSONArray& arr, const std::set<BSONType>* expectedTypes) {
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        if (bob.hasField("reason")) {
            return;  // there's already a reason for failure
        }
        if (!expectedTypes) {
            return;  // this operator accepts all types
        }
        for (auto&& elem : arr) {
            if (expectedTypes->count(elem.type())) {
                return;  // an element has one of the expected types
            }
        }
        bob.append("reason", "type did not match");
        appendConsideredTypes(arr);
        std::set<std::string> types;
        for (auto&& elem : *expectedTypes) {
            types.insert(typeName(elem));
        }
        if (types.size() == 1) {
            bob.append("expectedType", *types.begin());
        } else {
            bob.append("expectedTypes", types);
        }
    }
    void appendErrorReason(const MatchExpression& expr,
                           const std::string& normalReason,
                           const std::string& invertedReason) {
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        if (bob.hasField("reason")) {
            return;  // there's already a reason for failure
        }
        if (_context->inversion == InvertError::kNormal) {
            bob.append("reason", normalReason);
        } else {
            bob.append("reason", invertedReason);
        }
    }
    void appendConsideredValues(const BSONArray& arr) {
        int size = arr.nFields();
        if (size == 0) {
            return;  // there are no values to append
        }
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        if (size == 1) {
            bob.appendAs(arr[0], "consideredValue");
        } else {
            bob.append("consideredValues", arr);
        }
    }
    void appendConsideredTypes(const BSONArray& arr) {
        if (arr.nFields() == 0) {
            return;  // no values means no considered types
        }
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        std::set<std::string> types;
        for (auto&& elem : arr) {
            types.insert(typeName(elem.type()));
        }
        if (types.size() == 1) {
            bob.append("consideredType", *types.begin());
        } else {
            bob.append("consideredTypes", types);
        }
    }

    /**
     * Given a pointer to a PathMatchExpression 'expr', appends details to the current
     * BSONObjBuilder tracked by '_context' describing why the document failed to match against
     * 'expr'. In particular:
     * - Appends "reason: field was missing" if expr's path is missing from the document.
     * - Appends "reason: type did not match" along with 'expectedTypes' and 'consideredTypes' if
     * none of the values at expr's path match any of the types specified in 'expectedTypes'.
     * - Appends the specified 'reason' along with 'consideredValue' if the 'path' in the
     * document resolves to a single value.
     * - Appends the specified 'reason' along with 'consideredValues' if the 'path' in the
     * document resolves to an array of values that is implicitly traversed by 'expr'.
     */
    void generatePathError(const PathMatchExpression& expr,
                           const std::string& normalReason,
                           const std::string& invertedReason,
                           const std::set<BSONType>* expectedTypes = nullptr,
                           LeafArrayBehavior leafArrayBehavior = LeafArrayBehavior::kTraverse) {
        _context->pushNewFrame(expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(expr)) {
            appendErrorDetails(expr);
            ElementPath path(expr.path(), leafArrayBehavior);
            BSONArray arr = createValuesArray(path);
            appendMissingField(arr);
            appendTypeMismatch(arr, expectedTypes);
            appendErrorReason(expr, normalReason, invertedReason);
            appendConsideredValues(arr);
        }
    }

    void generateComparisonError(const ComparisonMatchExpression* expr) {
        static constexpr auto normalReason = "comparison failed";
        static constexpr auto invertedReason = "comparison succeeded";
        generatePathError(*expr, normalReason, invertedReason);
    }

    void generateElemMatchError(const ArrayMatchingMatchExpression* expr) {
        static constexpr auto kNormalReason = "array did not satisfy the child predicate";
        static constexpr auto kInvertedReason = "array did satisfy the child predicate";
        generateArrayError(expr, kNormalReason, kInvertedReason);
    }

    void generateArrayError(const ArrayMatchingMatchExpression* expr,
                            const std::string& normalReason,
                            const std::string& invertedReason) {
        static const std::set<BSONType> expectedTypes{BSONType::Array};
        generatePathError(
            *expr, normalReason, invertedReason, &expectedTypes, LeafArrayBehavior::kNoTraversal);
    }

    template <class T>
    void generateTypeError(const TypeMatchExpressionBase<T>* expr, LeafArrayBehavior behavior) {
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        static constexpr auto kNormalReason = "type did not match";
        static constexpr auto kInvertedReason = "type did match";
        if (_context->shouldGenerateError(*expr)) {
            appendErrorDetails(*expr);
            ElementPath path(expr->path(), behavior);
            BSONArray arr = createValuesArray(path);
            appendMissingField(arr);
            appendErrorReason(*expr, kNormalReason, kInvertedReason);
            appendConsideredValues(arr);
            appendConsideredTypes(arr);
        }
    }
    /**
     * Generates a document validation error for a bit test expression 'expr'.
     */
    void generateError(const BitTestMatchExpression* expr) {
        static constexpr auto kNormalReason = "bitwise operator failed to match";
        static constexpr auto kInvertedReason = "bitwise operator matched successfully";
        static const std::set<BSONType> kExpectedTypes{BSONType::NumberInt,
                                                       BSONType::NumberLong,
                                                       BSONType::NumberDouble,
                                                       BSONType::NumberDecimal,
                                                       BSONType::BinData};
        generatePathError(*expr, kNormalReason, kInvertedReason, &kExpectedTypes);
    }

    /**
     * Performs the setup necessary to generate an error for 'expr'.
     */
    void preVisitTreeOperator(const MatchExpression* expr) {
        invariant(expr->numChildren() > 0);
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            auto annotation = expr->getErrorAnnotation();
            appendOperatorName(*annotation, &_context->getCurrentObjBuilder());
            _context->getCurrentObjBuilder().appendElements(annotation->annotation);
        }
    }
    /**
     * Utility to generate an error for $all. Though $all is internally translated to an 'AND'
     * over some child expressions, it is treated as a leaf operator for the purposes of error
     * reporting.
     */
    void processAll(const AndMatchExpression& expr) {
        _context->pushNewFrame(expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(expr)) {
            invariant(expr.numChildren() > 0);
            appendErrorDetails(expr);
            auto childExpr = expr.getChild(0);
            static constexpr auto kNormalReason = "array did not contain all specified values";
            static constexpr auto kInvertedReason = "array did contain all specified values";
            ElementPath path(childExpr->path(), LeafArrayBehavior::kNoTraversal);
            auto arr = createValuesArray(path);
            appendMissingField(arr);
            appendErrorReason(expr, kNormalReason, kInvertedReason);
            appendConsideredValues(arr);
        }
    }

    /**
     * For an AlwaysBooleanMatchExpression, we simply output the error information obtained at
     * parse time.
     */
    void generateAlwaysBooleanError(const AlwaysBooleanMatchExpression& expr) {
        _context->pushNewFrame(expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(expr)) {
            // An AlwaysBooleanMatchExpression can only contribute to error generation when the
            // inversion matches the value of the 'expr'. More precisely, it is only possible
            // to generate an error for 'expr' if it evaluates to false in a normal context or
            // if it evaluates to true an inverted context.
            if (expr.isTriviallyFalse()) {
                invariant(_context->inversion == InvertError::kNormal);
            } else {
                invariant(_context->inversion == InvertError::kInverted);
            }
            appendErrorDetails(expr);
            static constexpr auto kNormalReason = "expression always evaluates to false";
            static constexpr auto kInvertedReason = "expression always evaluates to true";
            appendErrorReason(expr, kNormalReason, kInvertedReason);
        }
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
    void visit(const AndMatchExpression* expr) final {
        inVisitTreeOperator(expr);
    }
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
    void visit(const NorMatchExpression* expr) final {
        inVisitTreeOperator(expr);
    }
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {
        inVisitTreeOperator(expr);
    }
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
    void inVisitTreeOperator(const ListOfMatchExpression* expr) {
        finishLogicalOperatorChildError(expr, _context);
    }
    ValidationErrorContext* _context;
};

/**
 * Visitor which finalizes the generated error for the current MatchExpression.
 */
class ValidationErrorPostVisitor final : public MatchExpressionConstVisitor {
public:
    ValidationErrorPostVisitor(ValidationErrorContext* context) : _context(context) {}
    void visit(const AlwaysFalseMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const AndMatchExpression* expr) final {
        auto operatorName = expr->getErrorAnnotation()->operatorName;
        // Clean up the frame for this node if we're finishing the error for an $all or this node
        // shouldn't generate an error.
        if (operatorName == "$all" || !_context->shouldGenerateError(*expr)) {
            _context->finishCurrentError(expr);
            return;
        }
        // Specify a different details string based on the operatorName. Note that if our node
        // doesn't have an operator name specified, the default reason string is 'details'.
        static const StringMap<std::string> detailsStringMap = {
            {"$and", "clausesNotSatisfied"},
            {"properties", "propertiesNotSatisfied"},
            {"$jsonSchema", "schemaRulesNotSatisfied"},
            {"", "details"}};
        auto detailsString = detailsStringMap.find(operatorName);
        invariant(detailsString != detailsStringMap.end());
        postVisitTreeOperator(expr, detailsString->second);
    }
    void visit(const BitsAllClearMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ExistsMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ExprMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const GTEMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const GeoMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const InMatchExpression* expr) final {
        _context->finishCurrentError(expr);
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
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ModMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const NorMatchExpression* expr) final {
        _context->flipInversion();
        static constexpr auto detailsString = "clausesNotSatisfied";
        postVisitTreeOperator(expr, detailsString);
    }
    void visit(const NotMatchExpression* expr) final {
        _context->flipInversion();
        if (_context->shouldGenerateError(*expr)) {
            _context->getCurrentObjBuilder().append("details", _context->getLatestCompleteError());
        }
        _context->finishCurrentError(expr);
    }
    void visit(const OrMatchExpression* expr) final {
        static constexpr auto detailsString = "clausesNotSatisfied";
        postVisitTreeOperator(expr, detailsString);
    }
    void visit(const RegexMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const SizeMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const TextMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const WhereMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }

private:
    void postVisitTreeOperator(const ListOfMatchExpression* expr,
                               const std::string& detailsString) {
        finishLogicalOperatorChildError(expr, _context);
        if (_context->shouldGenerateError(*expr)) {
            auto failedClauses = _context->getCurrentArrayBuilder().arr();
            _context->getCurrentObjBuilder().append(detailsString, failedClauses);
        }
        _context->finishCurrentError(expr);
    }

    ValidationErrorContext* _context;
};

/**
 * Returns true if each node in the tree rooted at 'validatorExpr' has an error annotation, false
 * otherwise.
 */
bool hasErrorAnnotations(const MatchExpression& validatorExpr) {
    if (!validatorExpr.getErrorAnnotation())
        return false;
    for (const auto childExpr : validatorExpr) {
        if (!childExpr || !hasErrorAnnotations(*childExpr)) {
            return false;
        }
    }
    return true;
}

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
    ValidationErrorContext context(doc);
    ValidationErrorPreVisitor preVisitor{&context};
    ValidationErrorInVisitor inVisitor{&context};
    ValidationErrorPostVisitor postVisitor{&context};
    // TODO SERVER-49446: Once all nodes have ErrorAnnotations, this check should be converted to an
    // invariant check that all nodes have an annotation.
    if (!hasErrorAnnotations(validatorExpr)) {
        return BSONObj();
    }
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(&validatorExpr, &walker);

    // There should be no frames when error generation is complete as the finished error will be
    // stored in 'context'.
    invariant(context.frames.empty());
    return context.getLatestCompleteError();
}

}  // namespace mongo::doc_validation_error
