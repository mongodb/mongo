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

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/in_list_data.h"
#include "mongo/db/matcher/match_details.h"
#include "mongo/db/matcher/path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/pcre.h"

namespace mongo {

class CollatorInterface;

/**
 * Makes a conjunction of the given predicates
 */
template <typename... Ts>
inline auto makeAnd(Ts&&... pack) {
    auto predicates = makeVector<std::unique_ptr<MatchExpression>>(std::forward<Ts>(pack)...);
    return std::make_unique<AndMatchExpression>(std::move(predicates));
}

/**
 * Makes a disjunction of the given predicates.
 *
 * - The result is non-null; it may be an OrMatchExpression with zero children.
 * - Any trivially-false arguments are omitted.
 * - If only one argument is nontrivial, returns that argument rather than adding an extra
 *   OrMatchExpression around it.
 */
template <typename... Ts>
inline auto makeOr(Ts&&... pack) {
    auto predicates = makeVector<std::unique_ptr<MatchExpression>>(std::forward<Ts>(pack)...);
    auto newEnd = std::remove_if(
        predicates.begin(), predicates.end(), [](auto& node) { return node->isTriviallyFalse(); });
    predicates.erase(newEnd, predicates.end());
    return std::make_unique<OrMatchExpression>(std::move(predicates));
}

class LeafMatchExpression : public PathMatchExpression {
public:
    LeafMatchExpression(MatchType matchType,
                        boost::optional<StringData> path,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : LeafMatchExpression(matchType,
                              path,
                              ElementPath::LeafArrayBehavior::kTraverse,
                              ElementPath::NonLeafArrayBehavior::kTraverse,
                              std::move(annotation)) {}

    LeafMatchExpression(MatchType matchType,
                        boost::optional<StringData> path,
                        ElementPath::LeafArrayBehavior leafArrBehavior,
                        ElementPath::NonLeafArrayBehavior nonLeafArrBehavior,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : PathMatchExpression(
              matchType, path, leafArrBehavior, nonLeafArrBehavior, std::move(annotation)) {}

    virtual ~LeafMatchExpression() = default;

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        MONGO_UNREACHABLE_TASSERT(6400209);
    }


    void resetChild(size_t, MatchExpression*) override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    MatchCategory getCategory() const override {
        return MatchCategory::kLeaf;
    }
};

/**
 * Base class for comparison-like match expression nodes. This includes both the comparison nodes in
 * the match language ($eq, $gt, $gte, $lt, and $lte), as well as internal comparison nodes like
 * $_internalExprEq.
 */
class ComparisonMatchExpressionBase : public LeafMatchExpression {
public:
    static bool isEquality(MatchType matchType) {
        switch (matchType) {
            case MatchExpression::EQ:
            case MatchExpression::INTERNAL_EXPR_EQ:
                return true;
            default:
                return false;
        }
    }

    static bool isInternalExprComparison(MatchType matchType) {
        switch (matchType) {
            case MatchExpression::INTERNAL_EXPR_EQ:
            case MatchExpression::INTERNAL_EXPR_GT:
            case MatchExpression::INTERNAL_EXPR_GTE:
            case MatchExpression::INTERNAL_EXPR_LT:
            case MatchExpression::INTERNAL_EXPR_LTE:
                return true;
            default:
                return false;
        }
    }

    template <typename T>
    ComparisonMatchExpressionBase(MatchType type,
                                  boost::optional<StringData> path,
                                  T&& rhs,
                                  ElementPath::LeafArrayBehavior,
                                  ElementPath::NonLeafArrayBehavior,
                                  clonable_ptr<ErrorAnnotation> annotation = nullptr,
                                  const CollatorInterface* collator = nullptr);

    virtual ~ComparisonMatchExpressionBase() = default;

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    virtual void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                               const SerializationOptions& opts) const;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * Returns the name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    const BSONElement& getData() const {
        return _rhs;
    }

    /**
     * Replaces the RHS element of this expression. The caller is responsible for ensuring that the
     * BSONObj backing 'elem' outlives this MatchExpression.
     */
    void setData(BSONElement elem) {
        // TODO SERVER-50629: Ensure that the _backingBSON is consistent with the new element.
        _rhs = elem;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    void setInputParamId(boost::optional<InputParamId> paramId) {
        _inputParamId = paramId;
    }

    boost::optional<InputParamId> getInputParamId() const {
        return _inputParamId;
    }

protected:
    /**
     * 'collator' must outlive the ComparisonMatchExpression and any clones made of it.
     */
    void _doSetCollator(const CollatorInterface* collator) final {
        _collator = collator;
    }

    // BSON which holds the data referenced by _rhs.
    BSONObj _backingBSON;
    BSONElement _rhs;

    // Collator used to compare elements. By default, simple binary comparison will be used.
    const CollatorInterface* _collator = nullptr;

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) {
            return expression;
        };
    }

    boost::optional<InputParamId> _inputParamId;
};

/**
 * EQ, LTE, LT, GT, GTE subclass from ComparisonMatchExpression.
 */
class ComparisonMatchExpression : public ComparisonMatchExpressionBase {
public:
    /**
     * Returns true if the MatchExpression is a ComparisonMatchExpression.
     */
    static bool isComparisonMatchExpression(MatchExpression::MatchType matchType) {
        switch (matchType) {
            case MatchExpression::LT:
            case MatchExpression::LTE:
            case MatchExpression::EQ:
            case MatchExpression::GTE:
            case MatchExpression::GT:
                return true;
            default:
                return false;
        }
    }

    /**
     * Returns true if the MatchExpression is a ComparisonMatchExpression.
     */
    static bool isComparisonMatchExpression(const MatchExpression* expr) {
        return isComparisonMatchExpression(expr->matchType());
    }

    template <typename T>
    ComparisonMatchExpression(MatchType type,
                              boost::optional<StringData> path,
                              T&& rhs,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr,
                              const CollatorInterface* collator = nullptr);

    virtual ~ComparisonMatchExpression() = default;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;
};

class EqualityMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$eq"_sd;

    EqualityMatchExpression(boost::optional<StringData> path,
                            Value rhs,
                            clonable_ptr<ErrorAnnotation> annotation = nullptr,
                            const CollatorInterface* collator = nullptr)
        : ComparisonMatchExpression(EQ, path, std::move(rhs), std::move(annotation), collator) {}
    EqualityMatchExpression(boost::optional<StringData> path,
                            const BSONElement& rhs,
                            clonable_ptr<ErrorAnnotation> annotation = nullptr,
                            const CollatorInterface* collator = nullptr)
        : ComparisonMatchExpression(EQ, path, rhs, std::move(annotation), collator) {
        invariant(!rhs.eoo());
    }

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<ComparisonMatchExpression> e =
            std::make_unique<EqualityMatchExpression>(path(), getData(), _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        if (getInputParamId()) {
            e->setInputParamId(*getInputParamId());
        }
        return e;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class LTEMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$lte"_sd;

    LTEMatchExpression(boost::optional<StringData> path,
                       Value rhs,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(LTE, path, std::move(rhs), std::move(annotation)) {}
    LTEMatchExpression(boost::optional<StringData> path,
                       const BSONElement& rhs,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(LTE, path, rhs, std::move(annotation)) {
        invariant(!rhs.eoo());
    }

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<ComparisonMatchExpression> e =
            std::make_unique<LTEMatchExpression>(path(), _rhs, _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        if (getInputParamId()) {
            e->setInputParamId(*getInputParamId());
        }
        return e;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class LTMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$lt"_sd;

    LTMatchExpression(boost::optional<StringData> path,
                      Value rhs,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(LT, path, std::move(rhs), std::move(annotation)) {}
    LTMatchExpression(boost::optional<StringData> path,
                      const BSONElement& rhs,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(LT, path, rhs, std::move(annotation)) {
        invariant(!rhs.eoo());
    }

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<ComparisonMatchExpression> e =
            std::make_unique<LTMatchExpression>(path(), _rhs, _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        if (getInputParamId()) {
            e->setInputParamId(*getInputParamId());
        }
        return e;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    bool isLTMaxKey() const final {
        return _rhs.type() == BSONType::MaxKey;
    }
};

class GTMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$gt"_sd;

    GTMatchExpression(boost::optional<StringData> path,
                      Value rhs,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(GT, path, std::move(rhs), std::move(annotation)) {}

    GTMatchExpression(boost::optional<StringData> path,
                      const BSONElement& rhs,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(GT, path, rhs, std::move(annotation)) {
        invariant(!rhs.eoo());
    }

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<ComparisonMatchExpression> e =
            std::make_unique<GTMatchExpression>(path(), _rhs, _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        if (getInputParamId()) {
            e->setInputParamId(*getInputParamId());
        }
        return e;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    bool isGTMinKey() const final {
        return _rhs.type() == BSONType::MinKey;
    }
};

class GTEMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$gte"_sd;

    GTEMatchExpression(boost::optional<StringData> path,
                       Value rhs,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(GTE, path, std::move(rhs), std::move(annotation)) {}
    GTEMatchExpression(boost::optional<StringData> path,
                       const BSONElement& rhs,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(GTE, path, rhs, std::move(annotation)) {
        invariant(!rhs.eoo());
    }

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<ComparisonMatchExpression> e =
            std::make_unique<GTEMatchExpression>(path(), _rhs, _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        if (getInputParamId()) {
            e->setInputParamId(*getInputParamId());
        }
        return e;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class RegexMatchExpression : public LeafMatchExpression {
public:
    static const std::set<char> kValidRegexFlags;

    static std::unique_ptr<pcre::Regex> makeRegex(const std::string& regex,
                                                  const std::string& flags);

    RegexMatchExpression(boost::optional<StringData> path,
                         Value e,
                         clonable_ptr<ErrorAnnotation> annotation)
        : RegexMatchExpression(path, e.getRegex(), e.getRegexFlags(), std::move(annotation)) {}

    RegexMatchExpression(boost::optional<StringData> path,
                         const BSONElement& e,
                         clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : RegexMatchExpression(path, Value(e), annotation) {}

    RegexMatchExpression(boost::optional<StringData> path,
                         StringData regex,
                         StringData options,
                         clonable_ptr<ErrorAnnotation> annotation = nullptr);

    ~RegexMatchExpression();

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<RegexMatchExpression> e =
            std::make_unique<RegexMatchExpression>(path(), _regex, _flags, _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        if (getSourceRegexInputParamId()) {
            e->setSourceRegexInputParamId(*getSourceRegexInputParamId());
        }
        if (getCompiledRegexInputParamId()) {
            e->setCompiledRegexInputParamId(*getCompiledRegexInputParamId());
        }
        return e;
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const SerializationOptions& opts) const final;

    void serializeToBSONTypeRegex(BSONObjBuilder* out) const;

    void shortDebugString(StringBuilder& debug) const;

    virtual bool equivalent(const MatchExpression* other) const;

    const std::string& getString() const {
        return _regex;
    }
    const std::string& getFlags() const {
        return _flags;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void setSourceRegexInputParamId(boost::optional<InputParamId> paramId) {
        _sourceRegexInputParamId = paramId;
    }

    void setCompiledRegexInputParamId(boost::optional<InputParamId> paramId) {
        _compiledRegexInputParamId = paramId;
    }

    boost::optional<InputParamId> getSourceRegexInputParamId() const {
        return _sourceRegexInputParamId;
    }

    boost::optional<InputParamId> getCompiledRegexInputParamId() const {
        return _compiledRegexInputParamId;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) {
            return expression;
        };
    }

    void _init();

    std::string _regex;
    std::string _flags;
    std::unique_ptr<pcre::Regex> _re;

    boost::optional<InputParamId> _sourceRegexInputParamId;
    boost::optional<InputParamId> _compiledRegexInputParamId;
};

class ModMatchExpression : public LeafMatchExpression {
public:
    ModMatchExpression(boost::optional<StringData> path,
                       long long divisor,
                       long long remainder,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<ModMatchExpression> m =
            std::make_unique<ModMatchExpression>(path(), _divisor, _remainder, _errorAnnotation);
        if (getTag()) {
            m->setTag(getTag()->clone());
        }
        if (getDivisorInputParamId()) {
            m->setDivisorInputParamId(*getDivisorInputParamId());
        }
        if (getRemainderInputParamId()) {
            m->setRemainderInputParamId(*getRemainderInputParamId());
        }
        return m;
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const SerializationOptions& opts) const final;

    virtual bool equivalent(const MatchExpression* other) const;

    long long getDivisor() const {
        return _divisor;
    }
    long long getRemainder() const {
        return _remainder;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void setDivisorInputParamId(boost::optional<InputParamId> paramId) {
        _divisorInputParamId = paramId;
    }

    void setRemainderInputParamId(boost::optional<InputParamId> paramId) {
        _remainderInputParamId = paramId;
    }

    boost::optional<InputParamId> getDivisorInputParamId() const {
        return _divisorInputParamId;
    }

    boost::optional<InputParamId> getRemainderInputParamId() const {
        return _remainderInputParamId;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) {
            return expression;
        };
    }

    long long _divisor;
    long long _remainder;

    boost::optional<InputParamId> _divisorInputParamId;
    boost::optional<InputParamId> _remainderInputParamId;
};

class ExistsMatchExpression : public LeafMatchExpression {
public:
    explicit ExistsMatchExpression(boost::optional<StringData> path,
                                   clonable_ptr<ErrorAnnotation> annotation = nullptr);

    virtual std::unique_ptr<MatchExpression> clone() const {
        std::unique_ptr<ExistsMatchExpression> e =
            std::make_unique<ExistsMatchExpression>(path(), _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return e;
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const SerializationOptions& opts) const final;

    virtual bool equivalent(const MatchExpression* other) const;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) {
            return expression;
        };
    }
};

/**
 * query operator: $in
 */
class InMatchExpression : public LeafMatchExpression {
public:
    explicit InMatchExpression(boost::optional<StringData> path,
                               clonable_ptr<ErrorAnnotation> annotation = nullptr);

    explicit InMatchExpression(boost::optional<StringData> path,
                               clonable_ptr<ErrorAnnotation> annotation,
                               std::shared_ptr<InListData> equalities);

    std::unique_ptr<MatchExpression> clone() const final;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const SerializationOptions& opts) const final;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * 'collator' must outlive the InMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator);

    const std::vector<BSONElement>& getEqualities() const {
        return _equalities->getElements();
    }

    const std::shared_ptr<InListData>& getInList() const {
        return _equalities;
    }

    const std::vector<std::unique_ptr<RegexMatchExpression>>& getRegexes() const {
        return _regexes;
    }

    const CollatorInterface* getCollator() const {
        return _equalities->getCollator();
    }

    /**
     * Sets the equalities to 'bsonArray'. If 'bool(fn)' is true, then 'fn' will be invoked
     * for each element in 'bsonArray'. If 'fn' returns a non-OK Status for any element, this
     * function will immediately break and return that Status. If 'bool(fn)' is false, then
     * 'fn' is ignored.
     *
     * This function will throw an error if any value in 'bsonArray' is Undefined. Any Regex
     * values in 'bsonArray' will get passed to 'fn()' (assuming that 'bool(fn)' is true) but
     * otherwise will be ignored.
     */
    Status setEqualitiesArray(BSONObj bsonArray,
                              const std::function<Status(const BSONElement&)>& fn) {
        cloneEqualitiesBeforeWriteIfNeeded();
        constexpr bool errorOnRegex = false;
        return _equalities->setElementsArray(std::move(bsonArray), errorOnRegex, fn);
    }

    /**
     * Sets the equalities to 'bsonArray'. This function will throw an error if any value in
     * 'bsonArray' is Regex or Undefined.
     */
    Status setEqualitiesArray(BSONObj bsonArray) {
        cloneEqualitiesBeforeWriteIfNeeded();
        return _equalities->setElementsArray(std::move(bsonArray));
    }

    /**
     * Sets the equalities to 'equalities'. This function will throw an error if any value in
     * 'equalities' is Regex or Undefined.
     */
    Status setEqualities(std::vector<BSONElement> equalities) {
        cloneEqualitiesBeforeWriteIfNeeded();
        return _equalities->setElements(std::move(equalities));
    }

    Status addRegex(std::unique_ptr<RegexMatchExpression> expr);

    bool contains(const BSONElement& e) const {
        return _equalities->contains(e);
    }

    bool hasRegex() const {
        return !_regexes.empty();
    }

    bool hasNull() const {
        return _equalities->hasNull();
    }
    bool hasArray() const {
        return _equalities->hasArray();
    }
    bool hasObject() const {
        return _equalities->hasObject();
    }
    bool hasEmptyArray() const {
        return _equalities->hasEmptyArray();
    }
    bool hasEmptyObject() const {
        return _equalities->hasEmptyObject();
    }
    bool hasNonEmptyArray() const {
        return _equalities->hasNonEmptyArray();
    }
    bool hasNonEmptyObject() const {
        return _equalities->hasNonEmptyObject();
    }
    bool hasNonEmptyArrayOrObject() const {
        return hasNonEmptyArray() || hasNonEmptyObject();
    }
    bool hasNonScalarOrNonEmptyValues() const {
        return hasNonEmptyArrayOrObject() || hasNull() || hasRegex();
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void setInputParamId(boost::optional<InputParamId> paramId) {
        _inputParamId = paramId;
    }

    boost::optional<InputParamId> getInputParamId() const {
        return _inputParamId;
    }

private:
    inline void cloneEqualitiesBeforeWriteIfNeeded() {
        // If '_equalities' has been prepared then it can't be modified, so make a mutable copy
        // and then update '_equalities' to point to the copy.
        if (_equalities->isPrepared()) {
            _equalities = _equalities->clone();
        }
    }

    ExpressionOptimizerFunc getOptimizer() const final;

    // A helper to serialize to something like {$in: "?array<?number>"} or similar, depending on
    // 'opts' and whether we have a mixed-type $in or not.
    void serializeToShape(BSONObjBuilder* bob, const SerializationOptions& opts) const;

    // List of equalities (excluding regexes).
    std::shared_ptr<InListData> _equalities;

    // Container of regex elements this object owns.
    std::vector<std::unique_ptr<RegexMatchExpression>> _regexes;

    boost::optional<InputParamId> _inputParamId;
};

/**
 * Bit test query operators include $bitsAllSet, $bitsAllClear, $bitsAnySet, and $bitsAnyClear.
 */
class BitTestMatchExpression : public LeafMatchExpression {
public:
    /**
     * Construct with either bit positions, a 64-bit numeric bitmask, or a char array
     * bitmask.
     */
    explicit BitTestMatchExpression(MatchType type,
                                    boost::optional<StringData> path,
                                    std::vector<uint32_t> bitPositions,
                                    clonable_ptr<ErrorAnnotation> annotation);
    explicit BitTestMatchExpression(MatchType type,
                                    boost::optional<StringData> path,
                                    uint64_t bitMask,
                                    clonable_ptr<ErrorAnnotation> annotation);
    explicit BitTestMatchExpression(MatchType type,
                                    boost::optional<StringData> path,
                                    const char* bitMaskBinary,
                                    uint32_t bitMaskLen,
                                    clonable_ptr<ErrorAnnotation> annotation);
    virtual ~BitTestMatchExpression() {}

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const SerializationOptions& opts) const final;

    virtual bool equivalent(const MatchExpression* other) const;

    size_t numBitPositions() const {
        return _bitPositions.size();
    }

    const std::vector<uint32_t>& getBitPositions() const {
        return _bitPositions;
    }

    uint64_t getBitMask() const {
        return _bitMask;
    }

    std::string name() const;

    void setBitPositionsParamId(boost::optional<InputParamId> paramId) {
        _bitPositionsParamId = paramId;
    }

    void setBitMaskParamId(boost::optional<InputParamId> paramId) {
        _bitMaskParamId = paramId;
    }

    boost::optional<InputParamId> getBitPositionsParamId() const {
        return _bitPositionsParamId;
    }

    boost::optional<InputParamId> getBitMaskParamId() const {
        return _bitMaskParamId;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) {
            return expression;
        };
    }

    /**
     * Performs bit test using bit positions on 'eValue' and returns whether or not the bit test
     * passes.
     */
    bool performBitTest(long long eValue) const;

    /**
     * Performs bit test using bit positions on 'eBinary' with length (in bytes) 'eBinaryLen' and
     * returns whether or not the bit test passes.
     */
    bool performBitTest(const char* eBinary, uint32_t eBinaryLen) const;

    /**
     * Helper function for performBitTest(...).
     *
     * needFurtherBitTests() determines if the result of a bit-test ('isBitSet') is enough
     * information to skip the rest of the bit tests.
     **/
    bool needFurtherBitTests(bool isBitSet) const;

    // Vector of bit positions to test, with bit position 0 being the least significant bit.
    // Used to perform bit tests against BinData.
    std::vector<uint32_t> _bitPositions;

    // Used to perform bit tests against numbers using a single bitwise operation.
    uint64_t _bitMask = 0;

    // When this expression is parameterized, we require two parameter markers, not one: a parameter
    // marker for the vector of bit positions and a second for the bitmask. The runtime plan
    // needs both values so that it can operate against either BinData or numerical inputs.
    boost::optional<InputParamId> _bitPositionsParamId;
    boost::optional<InputParamId> _bitMaskParamId;
};

class BitsAllSetMatchExpression : public BitTestMatchExpression {
public:
    BitsAllSetMatchExpression(boost::optional<StringData> path,
                              std::vector<uint32_t> bitPositions,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ALL_SET, path, std::move(bitPositions), std::move(annotation)) {}

    BitsAllSetMatchExpression(boost::optional<StringData> path,
                              uint64_t bitMask,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ALL_SET, path, bitMask, std::move(annotation)) {}

    BitsAllSetMatchExpression(boost::optional<StringData> path,
                              const char* bitMaskBinary,
                              uint32_t bitMaskLen,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ALL_SET, path, bitMaskBinary, bitMaskLen, std::move(annotation)) {}

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            std::make_unique<BitsAllSetMatchExpression>(
                path(), getBitPositions(), _errorAnnotation);
        if (getTag()) {
            bitTestMatchExpression->setTag(getTag()->clone());
        }
        if (getBitPositionsParamId()) {
            bitTestMatchExpression->setBitPositionsParamId(*getBitPositionsParamId());
        }
        if (getBitMaskParamId()) {
            bitTestMatchExpression->setBitMaskParamId(*getBitMaskParamId());
        }
        return bitTestMatchExpression;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class BitsAllClearMatchExpression : public BitTestMatchExpression {
public:
    BitsAllClearMatchExpression(boost::optional<StringData> path,
                                std::vector<uint32_t> bitPositions,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ALL_CLEAR, path, std::move(bitPositions), std::move(annotation)) {}

    BitsAllClearMatchExpression(boost::optional<StringData> path,
                                uint64_t bitMask,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ALL_CLEAR, path, bitMask, std::move(annotation)) {}

    BitsAllClearMatchExpression(boost::optional<StringData> path,
                                const char* bitMaskBinary,
                                uint32_t bitMaskLen,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ALL_CLEAR, path, bitMaskBinary, bitMaskLen, std::move(annotation)) {}

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            std::make_unique<BitsAllClearMatchExpression>(
                path(), getBitPositions(), _errorAnnotation);
        if (getTag()) {
            bitTestMatchExpression->setTag(getTag()->clone());
        }
        if (getBitPositionsParamId()) {
            bitTestMatchExpression->setBitPositionsParamId(*getBitPositionsParamId());
        }
        if (getBitMaskParamId()) {
            bitTestMatchExpression->setBitMaskParamId(*getBitMaskParamId());
        }
        return bitTestMatchExpression;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class BitsAnySetMatchExpression : public BitTestMatchExpression {
public:
    BitsAnySetMatchExpression(boost::optional<StringData> path,
                              std::vector<uint32_t> bitPositions,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ANY_SET, path, std::move(bitPositions), std::move(annotation)) {}

    BitsAnySetMatchExpression(boost::optional<StringData> path,
                              uint64_t bitMask,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ANY_SET, path, bitMask, std::move(annotation)) {}

    BitsAnySetMatchExpression(boost::optional<StringData> path,
                              const char* bitMaskBinary,
                              uint32_t bitMaskLen,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ANY_SET, path, bitMaskBinary, bitMaskLen, std::move(annotation)) {}

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            std::make_unique<BitsAnySetMatchExpression>(
                path(), getBitPositions(), _errorAnnotation);
        if (getTag()) {
            bitTestMatchExpression->setTag(getTag()->clone());
        }
        if (getBitPositionsParamId()) {
            bitTestMatchExpression->setBitPositionsParamId(*getBitPositionsParamId());
        }
        if (getBitMaskParamId()) {
            bitTestMatchExpression->setBitMaskParamId(*getBitMaskParamId());
        }
        return bitTestMatchExpression;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class BitsAnyClearMatchExpression : public BitTestMatchExpression {
public:
    BitsAnyClearMatchExpression(boost::optional<StringData> path,
                                std::vector<uint32_t> bitPositions,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ANY_CLEAR, path, std::move(bitPositions), std::move(annotation)) {}

    BitsAnyClearMatchExpression(boost::optional<StringData> path,
                                uint64_t bitMask,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ANY_CLEAR, path, bitMask, std::move(annotation)) {}

    BitsAnyClearMatchExpression(boost::optional<StringData> path,
                                const char* bitMaskBinary,
                                uint32_t bitMaskLen,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ANY_CLEAR, path, bitMaskBinary, bitMaskLen, std::move(annotation)) {}

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            std::make_unique<BitsAnyClearMatchExpression>(
                path(), getBitPositions(), _errorAnnotation);
        if (getTag()) {
            bitTestMatchExpression->setTag(getTag()->clone());
        }
        if (getBitPositionsParamId()) {
            bitTestMatchExpression->setBitPositionsParamId(*getBitPositionsParamId());
        }
        if (getBitMaskParamId()) {
            bitTestMatchExpression->setBitMaskParamId(*getBitMaskParamId());
        }
        return bitTestMatchExpression;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};
}  // namespace mongo
