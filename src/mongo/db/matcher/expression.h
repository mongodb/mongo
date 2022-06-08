/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include <boost/optional.hpp>
#include <functional>
#include <memory>

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/match_details.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/util/fail_point.h"

namespace mongo {

/**
 * Enabling the disableMatchExpressionOptimization fail point will stop match expressions from
 * being optimized.
 */
extern FailPoint disableMatchExpressionOptimization;

class CollatorInterface;
class MatchExpression;
class TreeMatchExpression;

typedef StatusWith<std::unique_ptr<MatchExpression>> StatusWithMatchExpression;

class MatchExpression {
    MatchExpression(const MatchExpression&) = delete;
    MatchExpression& operator=(const MatchExpression&) = delete;

public:
    enum MatchType {
        // tree types
        AND,
        OR,

        // array types
        ELEM_MATCH_OBJECT,
        ELEM_MATCH_VALUE,
        SIZE,

        // leaf types
        EQ,
        LTE,
        LT,
        GT,
        GTE,
        REGEX,
        MOD,
        EXISTS,
        MATCH_IN,
        BITS_ALL_SET,
        BITS_ALL_CLEAR,
        BITS_ANY_SET,
        BITS_ANY_CLEAR,
        ENCRYPTED_BETWEEN,

        // Negations.
        NOT,
        NOR,

        // special types
        TYPE_OPERATOR,
        GEO,
        WHERE,
        EXPRESSION,

        // Boolean expressions.
        ALWAYS_FALSE,
        ALWAYS_TRUE,

        // Things that we parse but cannot be answered without an index.
        GEO_NEAR,
        TEXT,

        // Expressions that are only created internally
        INTERNAL_2D_POINT_IN_ANNULUS,
        INTERNAL_BUCKET_GEO_WITHIN,

        // Used to represent expression language comparisons in a match expression tree, since $eq,
        // $gt, $gte, $lt and $lte in the expression language has different semantics than their
        // respective match expressions.
        INTERNAL_EXPR_EQ,
        INTERNAL_EXPR_GT,
        INTERNAL_EXPR_GTE,
        INTERNAL_EXPR_LT,
        INTERNAL_EXPR_LTE,

        // JSON Schema expressions.
        INTERNAL_SCHEMA_ALLOWED_PROPERTIES,
        INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX,
        INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE,
        INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE,
        INTERNAL_SCHEMA_BIN_DATA_SUBTYPE,
        INTERNAL_SCHEMA_COND,
        INTERNAL_SCHEMA_EQ,
        INTERNAL_SCHEMA_FMOD,
        INTERNAL_SCHEMA_MATCH_ARRAY_INDEX,
        INTERNAL_SCHEMA_MAX_ITEMS,
        INTERNAL_SCHEMA_MAX_LENGTH,
        INTERNAL_SCHEMA_MAX_PROPERTIES,
        INTERNAL_SCHEMA_MIN_ITEMS,
        INTERNAL_SCHEMA_MIN_LENGTH,
        INTERNAL_SCHEMA_MIN_PROPERTIES,
        INTERNAL_SCHEMA_OBJECT_MATCH,
        INTERNAL_SCHEMA_ROOT_DOC_EQ,
        INTERNAL_SCHEMA_TYPE,
        INTERNAL_SCHEMA_UNIQUE_ITEMS,
        INTERNAL_SCHEMA_XOR,

    };

    /**
     * An iterator to walk through the children expressions of the given MatchExpressions. Along
     * with the defined 'begin()' and 'end()' functions, which take a reference to a
     * MatchExpression, this iterator can be used with a range-based loop. For example,
     *
     *    const MatchExpression* expr = makeSomeExpression();
     *    for (const auto& child : *expr) {
     *       ...
     *    }
     *
     * When incrementing the iterator, no checks are made to ensure the iterator does not pass
     * beyond the boundary. The caller is responsible to compare the iterator against an iterator
     * referring to the past-the-end child in the given expression, which can be obtained using
     * the 'mongo::end(*expr)' call.
     */
    template <bool IsConst>
    class MatchExpressionIterator {
    public:
        MatchExpressionIterator(const MatchExpression* expr, size_t index)
            : _expr(expr), _index(index) {}

        template <bool WasConst, typename = std::enable_if_t<IsConst && !WasConst>>
        MatchExpressionIterator(const MatchExpressionIterator<WasConst>& other)
            : _expr(other._expr), _index(other._index) {}

        template <bool WasConst, typename = std::enable_if_t<IsConst && !WasConst>>
        MatchExpressionIterator& operator=(const MatchExpressionIterator<WasConst>& other) {
            _expr = other._expr;
            _index = other._index;
            return *this;
        }

        MatchExpressionIterator& operator++() {
            ++_index;
            return *this;
        }

        MatchExpressionIterator operator++(int) {
            const auto ret{*this};
            ++(*this);
            return ret;
        }

        bool operator==(const MatchExpressionIterator& other) const {
            return _expr == other._expr && _index == other._index;
        }

        bool operator!=(const MatchExpressionIterator& other) const {
            return !(*this == other);
        }

        template <bool Const = IsConst>
        auto operator*() const -> std::enable_if_t<!Const, MatchExpression*> {
            return _expr->getChild(_index);
        }

        template <bool Const = IsConst>
        auto operator*() const -> std::enable_if_t<Const, const MatchExpression*> {
            return _expr->getChild(_index);
        }

    private:
        const MatchExpression* _expr;
        size_t _index;
    };

    using Iterator = MatchExpressionIterator<false>;
    using ConstIterator = MatchExpressionIterator<true>;
    using InputParamId = int32_t;

    /**
     * Tracks the information needed to generate a document validation error for a
     * MatchExpression node.
     */
    struct ErrorAnnotation {
        /**
         * Enumerated type describing what action to take when generating errors for document
         * validation failures.
         */
        enum class Mode {
            // Do not generate an error for this MatchExpression or any of its children.
            kIgnore,
            // Do not generate an error for this MatchExpression, but iterate over any children
            // as they may provide useful information. This is particularly useful for translated
            // jsonSchema keywords.
            kIgnoreButDescend,
            // Generate an error message.
            kGenerateError,
        };

        /**
         * JSON Schema annotations - 'title' and 'description' attributes.
         */
        struct SchemaAnnotations {
            /**
             * Constructs JSON schema annotations with annotation fields not set.
             */
            SchemaAnnotations() {}

            /**
             * Constructs JSON schema annotations from JSON Schema element 'jsonSchemaElement'.
             */
            SchemaAnnotations(const BSONObj& jsonSchemaElement);

            void appendElements(BSONObjBuilder& builder) const;

            boost::optional<std::string> title;
            boost::optional<std::string> description;
        };

        /**
         * Constructs an annotation for a MatchExpression which does not contribute to error output.
         */
        ErrorAnnotation(Mode mode)
            : tag(""), annotation(BSONObj()), mode(mode), schemaAnnotations(SchemaAnnotations()) {
            invariant(mode != Mode::kGenerateError);
        }

        /**
         * Constructs a complete annotation for a MatchExpression which contributes to error output.
         */
        ErrorAnnotation(std::string tag, BSONObj annotation, BSONObj schemaAnnotationsObj)
            : tag(std::move(tag)),
              annotation(annotation.getOwned()),
              mode(Mode::kGenerateError),
              schemaAnnotations(SchemaAnnotations(schemaAnnotationsObj)) {}

        std::unique_ptr<ErrorAnnotation> clone() const {
            return std::make_unique<ErrorAnnotation>(*this);
        }

        // Tracks either the name of a user facing MQL operator or an internal name for some logical
        // entity to be used for dispatching to the correct handling logic during error generation.
        // All internal names are denoted by an underscore prefix.
        const std::string tag;
        // Tracks the original expression as specified by the user.
        const BSONObj annotation;
        const Mode mode;
        const SchemaAnnotations schemaAnnotations;
    };

    /**
     * Make simplifying changes to the structure of a MatchExpression tree without altering its
     * semantics. This function may return:
     *   - a pointer to the original, unmodified MatchExpression,
     *   - a pointer to the original MatchExpression that has been mutated, or
     *   - a pointer to a new MatchExpression.
     *
     * The value of 'expression' must not be nullptr.
     */
    static std::unique_ptr<MatchExpression> optimize(std::unique_ptr<MatchExpression> expression) {
        // If the disableMatchExpressionOptimization failpoint is enabled, optimizations are skipped
        // and the expression is left unmodified.
        if (MONGO_unlikely(disableMatchExpressionOptimization.shouldFail())) {
            return expression;
        }

        auto optimizer = expression->getOptimizer();

        try {
            return optimizer(std::move(expression));
        } catch (DBException& ex) {
            ex.addContext("Failed to optimize expression");
            throw;
        }
    }

    /**
     * Traverses expression tree post-order. Sorts children at each non-leaf node by (MatchType,
     * path(), children, number of children).
     */
    static void sortTree(MatchExpression* tree);

    /**
     * Convenience method which normalizes a MatchExpression tree by optimizing and then sorting it.
     */
    static std::unique_ptr<MatchExpression> normalize(std::unique_ptr<MatchExpression> tree) {
        tree = optimize(std::move(tree));
        sortTree(tree.get());
        return tree;
    }

    /**
     * Assigns an optional input parameter ID to each node which is eligible for
     * auto-parameterization.
     *
     * Returns a map to a parameterized MatchExpression from assigned InputParamId.
     */
    static std::vector<const MatchExpression*> parameterize(MatchExpression* tree);

    MatchExpression(MatchType type, clonable_ptr<ErrorAnnotation> annotation = nullptr);
    virtual ~MatchExpression() {}

    //
    // Structural/AST information
    //

    /**
     * What type is the node?  See MatchType above.
     */
    MatchType matchType() const {
        return _matchType;
    }

    /**
     * Returns the number of child MatchExpression nodes contained by this node. It is expected that
     * a node that does not support child nodes will return 0.
     */
    virtual size_t numChildren() const = 0;

    /**
     * Returns the child of the current node at zero-based position 'index'. 'index' must be within
     * the range of [0, numChildren()).
     */
    virtual MatchExpression* getChild(size_t index) const = 0;


    /**
     * Delegates to the specified child unique_ptr's reset() method in order to replace child
     * expressions while traversing the tree.
     */
    virtual void resetChild(size_t index, MatchExpression* other) = 0;

    /**
     * For MatchExpression nodes that can participate in tree restructuring (like AND/OR),
     * returns a non-const vector of MatchExpression* child nodes. If the MatchExpression does
     * not participated in tree restructuring, returns boost::none. Do not use to traverse the
     * MatchExpression tree. Use numChildren() and getChild(), which provide access to all
     * nodes.
     */
    virtual std::vector<std::unique_ptr<MatchExpression>>* getChildVector() = 0;

    /**
     * Get the path of the leaf.  Returns StringData() if there is no path (node is logical).
     */
    virtual StringData path() const {
        return StringData();
    }
    /**
     * Similar to path(), but returns a FieldRef. Returns nullptr if there is no path.
     */
    virtual const FieldRef* fieldRef() const {
        return nullptr;
    }

    enum class MatchCategory {
        // Expressions that are leaves on the AST, these do not have any children.
        kLeaf,
        // Logical Expressions such as $and, $or, etc. that do not have a path and may have
        // one or more children.
        kLogical,
        // Expressions that operate on arrays only.
        kArrayMatching,
        // Expressions that don't fall into any particular bucket.
        kOther,
    };

    virtual MatchCategory getCategory() const = 0;

    /**
     * This method will perform a clone of the entire match expression tree, but will not clone the
     * memory pointed to by underlying BSONElements. To perform a "deep clone" use this method and
     * also ensure that the buffer held by the underlying BSONObj will not be destroyed during the
     * lifetime of the clone.
     */
    virtual std::unique_ptr<MatchExpression> shallowClone() const = 0;

    // XXX document
    virtual bool equivalent(const MatchExpression* other) const = 0;

    //
    // Determine if a document satisfies the tree-predicate.
    //

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const = 0;

    virtual bool matchesBSON(const BSONObj& doc, MatchDetails* details = nullptr) const;

    /**
     * Determines if 'elem' would satisfy the predicate if wrapped with the top-level field name of
     * the predicate. Does not check that the predicate has a single top-level field name. For
     * example, given the object obj={a: [5]}, the predicate {i: {$gt: 0}} would match the element
     * obj["a"]["0"] because it performs the match as if the element at "a.0" were the BSONObj {i:
     * 5}.
     */
    virtual bool matchesBSONElement(BSONElement elem, MatchDetails* details = nullptr) const;

    /**
     * Determines if the element satisfies the tree-predicate.
     * Not valid for all expressions (e.g. $where); in those cases, returns false.
     */
    virtual bool matchesSingleElement(const BSONElement& e,
                                      MatchDetails* details = nullptr) const = 0;

    //
    // Tagging mechanism: Hang data off of the tree for retrieval later.
    //

    class TagData {
    public:
        enum class Type { IndexTag, RelevantTag, OrPushdownTag };
        virtual ~TagData() {}
        virtual void debugString(StringBuilder* builder) const = 0;
        virtual TagData* clone() const = 0;
        virtual Type getType() const = 0;
    };

    /**
     * Takes ownership
     */
    void setTag(TagData* data) {
        _tagData.reset(data);
    }
    TagData* getTag() const {
        return _tagData.get();
    }
    virtual void resetTag() {
        setTag(nullptr);
        for (size_t i = 0; i < numChildren(); ++i) {
            getChild(i)->resetTag();
        }
    }

    /**
     * Set the collator 'collator' on this match expression and all its children.
     *
     * 'collator' must outlive the match expression.
     */
    void setCollator(const CollatorInterface* collator);

    /**
     * Add the fields required for matching to 'deps'.
     */
    void addDependencies(DepsTracker* deps) const;

    /**
     * Serialize the MatchExpression to BSON, appending to 'out'. Output of this method is expected
     * to be a valid query object, that, when parsed, produces a logically equivalent
     * MatchExpression. If 'includePath' is false then the serialization should assume it's in a
     * context where the path has been serialized elsewhere, such as within an $elemMatch value.
     */
    virtual void serialize(BSONObjBuilder* out, bool includePath = true) const = 0;

    /**
     * Convenience method which serializes this MatchExpression to a BSONObj.
     */
    BSONObj serialize(bool includePath = true) const {
        BSONObjBuilder bob;
        serialize(&bob, includePath);
        return bob.obj();
    }

    /**
     * Returns true if this expression will always evaluate to false, such as an $or with no
     * children.
     */
    virtual bool isTriviallyFalse() const {
        return false;
    }

    /**
     * Returns true if this expression will always evaluate to true, such as an $and with no
     * children.
     */
    virtual bool isTriviallyTrue() const {
        return false;
    }

    virtual bool isGTMinKey() const {
        return false;
    }

    virtual bool isLTMaxKey() const {
        return false;
    }

    virtual void acceptVisitor(MatchExpressionMutableVisitor* visitor) = 0;
    virtual void acceptVisitor(MatchExpressionConstVisitor* visitor) const = 0;

    // Returns nullptr if this MatchExpression node has no annotation.
    ErrorAnnotation* getErrorAnnotation() const {
        return _errorAnnotation.get();
    }

    void setErrorAnnotation(clonable_ptr<ErrorAnnotation> annotation) {
        _errorAnnotation = std::move(annotation);
    }

    //
    // Debug information
    //

    /**
     * Returns a debug string representing the match expression tree, including any tags attached
     * for planning. This debug string format may spill across multiple lines, so it is not suitable
     * for logging at low debug levels or for error messages.
     */
    std::string debugString() const;
    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const = 0;

    /**
     * Serializes this MatchExpression to BSON, and then returns a standard string representation of
     * the resulting BSON object.
     */
    std::string toString() const;

protected:
    /**
     * An ExpressionOptimizerFunc implements tree simplifications for a MatchExpression tree with a
     * specific type of MatchExpression at the root. Except for requiring a specific MatchExpression
     * subclass, an ExpressionOptimizerFunc has the same requirements and functionality as described
     * in the specification of MatchExpression::getOptimizer(std::unique_ptr<MatchExpression>).
     */
    using ExpressionOptimizerFunc =
        std::function<std::unique_ptr<MatchExpression>(std::unique_ptr<MatchExpression>)>;

    /**
     * Subclasses that are collation-aware must implement this method in order to capture changes
     * to the collator that occur after initialization time.
     */
    virtual void _doSetCollator(const CollatorInterface* collator){};

    virtual void _doAddDependencies(DepsTracker* deps) const {}

    void _debugAddSpace(StringBuilder& debug, int indentationLevel) const;

    clonable_ptr<ErrorAnnotation> _errorAnnotation;

private:
    /**
     * Subclasses should implement this function to provide an ExpressionOptimizerFunc specific to
     * the subclass. This function is only called by
     * MatchExpression::optimize(std::unique_ptr<MatchExpression>), which is responsible for calling
     * MatchExpression::getOptimizer() on its input MatchExpression and then passing the same
     * MatchExpression to the resulting ExpressionOptimizerFunc. There should be no other callers
     * to this function.
     *
     * Any MatchExpression subclass that stores child MatchExpression objects is responsible for
     * returning an ExpressionOptimizerFunc that recursively calls
     * MatchExpression::optimize(std::unique_ptr<MatchExpression>) on those children.
     *
     * See the descriptions of MatchExpression::optimize(std::unique_ptr<MatchExpression>) and
     * ExpressionOptimizerFunc for additional explanation of their interfaces and functionality.
     */
    virtual ExpressionOptimizerFunc getOptimizer() const = 0;

    MatchType _matchType;
    std::unique_ptr<TagData> _tagData;
};

inline MatchExpression::Iterator begin(MatchExpression& expr) {
    return {&expr, 0};
}

inline MatchExpression::ConstIterator begin(const MatchExpression& expr) {
    return {&expr, 0};
}

inline MatchExpression::Iterator end(MatchExpression& expr) {
    return {&expr, expr.numChildren()};
}

inline MatchExpression::ConstIterator end(const MatchExpression& expr) {
    return {&expr, expr.numChildren()};
}

}  // namespace mongo
