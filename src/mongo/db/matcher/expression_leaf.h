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
#include <memory>

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/pcre.h"

namespace mongo {

class CollatorInterface;

/**
 *  A struct primarily used to make input parameters for makePredicate() function. The
 * 'MatchExprType' helps in determining which MatchExpression to make in makePredicate() function.
 */
template <typename MatchExprType, typename ValueType = BSONElement>
struct MatchExprPredicate {
    MatchExprPredicate(StringData path_, ValueType value_) : path(path_), value(value_){};
    StringData path;
    ValueType value;
};

/**
 * Helper function to make $and predicates based on the set of predicates passed as parameters.
 */
template <typename T, typename ValueType, typename... Targs, typename... ValueTypeArgs>
std::unique_ptr<MatchExpression> makePredicate(
    MatchExprPredicate<T, ValueType> predicate,
    MatchExprPredicate<Targs, ValueTypeArgs>... predicates) {
    if constexpr (sizeof...(predicates) > 0) {
        return std::make_unique<AndMatchExpression>(makeVector<std::unique_ptr<MatchExpression>>(
            std::make_unique<T>(predicate.path, predicate.value),
            (std::make_unique<Targs>(predicates.path, predicates.value))...));
    } else {
        return std::make_unique<T>(predicate.path, predicate.value);
    }
}

class LeafMatchExpression : public PathMatchExpression {
public:
    LeafMatchExpression(MatchType matchType,
                        StringData path,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : LeafMatchExpression(matchType,
                              path,
                              ElementPath::LeafArrayBehavior::kTraverse,
                              ElementPath::NonLeafArrayBehavior::kTraverse,
                              std::move(annotation)) {}

    LeafMatchExpression(MatchType matchType,
                        StringData path,
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

    ComparisonMatchExpressionBase(MatchType type,
                                  StringData path,
                                  Value rhs,
                                  ElementPath::LeafArrayBehavior,
                                  ElementPath::NonLeafArrayBehavior,
                                  clonable_ptr<ErrorAnnotation> annotation = nullptr,
                                  const CollatorInterface* collator = nullptr);

    virtual ~ComparisonMatchExpressionBase() = default;

    virtual void debugString(StringBuilder& debug, int indentationLevel = 0) const;

    BSONObj getSerializedRightHandSide() const final;

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

    void setInputParamId(InputParamId paramId) {
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
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
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

    ComparisonMatchExpression(MatchType type,
                              StringData path,
                              Value rhs,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr,
                              const CollatorInterface* collator = nullptr);

    virtual ~ComparisonMatchExpression() = default;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;
};

class EqualityMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$eq"_sd;

    EqualityMatchExpression(StringData path,
                            Value rhs,
                            clonable_ptr<ErrorAnnotation> annotation = nullptr,
                            const CollatorInterface* collator = nullptr)
        : ComparisonMatchExpression(EQ, path, std::move(rhs), std::move(annotation), collator) {}
    EqualityMatchExpression(StringData path,
                            const BSONElement& rhs,
                            clonable_ptr<ErrorAnnotation> annotation = nullptr,
                            const CollatorInterface* collator = nullptr)
        : ComparisonMatchExpression(EQ, path, Value(rhs), std::move(annotation), collator) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        std::unique_ptr<ComparisonMatchExpression> e =
            std::make_unique<EqualityMatchExpression>(path(), Value(getData()), _errorAnnotation);
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

    LTEMatchExpression(StringData path,
                       Value rhs,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(LTE, path, std::move(rhs), std::move(annotation)) {}
    LTEMatchExpression(StringData path,
                       const BSONElement& rhs,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(LTE, path, Value(rhs), std::move(annotation)) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
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

    LTMatchExpression(StringData path,
                      Value rhs,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(LT, path, std::move(rhs), std::move(annotation)) {}
    LTMatchExpression(StringData path,
                      const BSONElement& rhs,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(LT, path, Value(rhs), std::move(annotation)) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
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

    GTMatchExpression(StringData path,
                      Value rhs,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(GT, path, std::move(rhs), std::move(annotation)) {}

    GTMatchExpression(StringData path,
                      const BSONElement& rhs,
                      clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(GT, path, Value(rhs), std::move(annotation)) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
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

    GTEMatchExpression(StringData path,
                       Value rhs,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(GTE, path, std::move(rhs), std::move(annotation)) {}
    GTEMatchExpression(StringData path,
                       const BSONElement& rhs,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ComparisonMatchExpression(GTE, path, Value(rhs), std::move(annotation)) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
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

    RegexMatchExpression(StringData path, Value e, clonable_ptr<ErrorAnnotation> annotation)
        : RegexMatchExpression(path, e.getRegex(), e.getRegexFlags(), std::move(annotation)) {}

    RegexMatchExpression(StringData path,
                         const BSONElement& e,
                         clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : RegexMatchExpression(path, Value(e), annotation) {}

    RegexMatchExpression(StringData path,
                         StringData regex,
                         StringData options,
                         clonable_ptr<ErrorAnnotation> annotation = nullptr);

    ~RegexMatchExpression();

    std::unique_ptr<MatchExpression> shallowClone() const final {
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

    BSONObj getSerializedRightHandSide() const final;

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

    void setSourceRegexInputParamId(InputParamId paramId) {
        _sourceRegexInputParamId = paramId;
    }

    void setCompiledRegexInputParamId(InputParamId paramId) {
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
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
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
    ModMatchExpression(StringData path,
                       long long divisor,
                       long long remainder,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> shallowClone() const final {
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

    BSONObj getSerializedRightHandSide() const final;

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

    void setDivisorInputParamId(InputParamId paramId) {
        _divisorInputParamId = paramId;
    }

    void setRemainderInputParamId(InputParamId paramId) {
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
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    long long _divisor;
    long long _remainder;

    boost::optional<InputParamId> _divisorInputParamId;
    boost::optional<InputParamId> _remainderInputParamId;
};

class ExistsMatchExpression : public LeafMatchExpression {
public:
    explicit ExistsMatchExpression(StringData path,
                                   clonable_ptr<ErrorAnnotation> annotation = nullptr);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ExistsMatchExpression> e =
            std::make_unique<ExistsMatchExpression>(path(), _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return e;
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    BSONObj getSerializedRightHandSide() const final;

    virtual bool equivalent(const MatchExpression* other) const;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }
};

/**
 * query operator: $in
 */
class InMatchExpression : public LeafMatchExpression {
public:
    explicit InMatchExpression(StringData path, clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> shallowClone() const final;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    BSONObj getSerializedRightHandSide() const final;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * 'collator' must outlive the InMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator);

    Status setEqualities(std::vector<BSONElement> equalities);

    void setBackingBSON(BSONObj equalityStorage);

    Status addRegex(std::unique_ptr<RegexMatchExpression> expr);

    const std::vector<BSONElement>& getEqualities() const {
        return _equalitySet;
    }

    bool contains(const BSONElement& e) const;

    const std::vector<std::unique_ptr<RegexMatchExpression>>& getRegexes() const {
        return _regexes;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    bool hasNull() const {
        return _hasNull;
    }

    bool hasRegex() const {
        return !_regexes.empty();
    }

    bool hasEmptyArray() const {
        return _hasEmptyArray;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void setInputParamId(InputParamId paramId) {
        _inputParamId = paramId;
    }

    boost::optional<InputParamId> getInputParamId() const {
        return _inputParamId;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    // Whether or not '_equalities' has a jstNULL element in it.
    bool _hasNull = false;

    // Whether or not '_equalities' has an empty array element in it.
    bool _hasEmptyArray = false;

    // Collator used to construct '_eltCmp';
    const CollatorInterface* _collator = nullptr;

    // Comparator used to compare elements. By default, simple binary comparison will be used.
    BSONElementComparator _eltCmp;

    // Original container of equality elements, including duplicates. Needed for re-computing
    // '_equalitySet' in case '_collator' changes after elements have been added.
    //
    // We keep the equalities in sorted order according to the current BSON element comparator. This
    // enables a fast-path to avoid re-sorting if the expression is serialized and re-parsed.
    std::vector<BSONElement> _originalEqualityVector;

    // Deduped set of equality elements associated with this expression. Kept in sorted order to
    // support std::binary_search. Because we need to sort the elements anyway for things like index
    // bounds building, using binary search avoids the overhead of inserting into a hash table which
    // doesn't pay for itself in the common case where lookups are done a few times if ever.
    // TODO It may be worth dynamically creating a hashset after matchesSingleElement() has been
    // called "many" times.
    std::vector<BSONElement> _equalitySet;

    // Container of regex elements this object owns.
    std::vector<std::unique_ptr<RegexMatchExpression>> _regexes;

    // When this $in is generated internally, e.g. via a rewrite, this is where we store the
    // data of the corresponding equality elements.
    BSONObj _equalityStorage;

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
                                    StringData path,
                                    std::vector<uint32_t> bitPositions,
                                    clonable_ptr<ErrorAnnotation> annotation);
    explicit BitTestMatchExpression(MatchType type,
                                    StringData path,
                                    uint64_t bitMask,
                                    clonable_ptr<ErrorAnnotation> annotation);
    explicit BitTestMatchExpression(MatchType type,
                                    StringData path,
                                    const char* bitMaskBinary,
                                    uint32_t bitMaskLen,
                                    clonable_ptr<ErrorAnnotation> annotation);
    virtual ~BitTestMatchExpression() {}

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int indentationLevel) const;

    BSONObj getSerializedRightHandSide() const final;

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

    void setBitPositionsParamId(InputParamId paramId) {
        _bitPositionsParamId = paramId;
    }

    void setBitMaskParamId(InputParamId paramId) {
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
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
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

    // When this expression is parameterized, we require two parmeter markers, not one: a parameter
    // marker for the vector of bit positions and a second for the bitmask. The runtime plan
    // needs both values so that it can operate against either BinData or numerical inputs.
    boost::optional<InputParamId> _bitPositionsParamId;
    boost::optional<InputParamId> _bitMaskParamId;
};

class BitsAllSetMatchExpression : public BitTestMatchExpression {
public:
    BitsAllSetMatchExpression(StringData path,
                              std::vector<uint32_t> bitPositions,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ALL_SET, path, bitPositions, std::move(annotation)) {}

    BitsAllSetMatchExpression(StringData path,
                              uint64_t bitMask,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ALL_SET, path, bitMask, std::move(annotation)) {}

    BitsAllSetMatchExpression(StringData path,
                              const char* bitMaskBinary,
                              uint32_t bitMaskLen,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ALL_SET, path, bitMaskBinary, bitMaskLen, std::move(annotation)) {}

    std::unique_ptr<MatchExpression> shallowClone() const final {
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
    BitsAllClearMatchExpression(StringData path,
                                std::vector<uint32_t> bitPositions,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ALL_CLEAR, path, bitPositions, std::move(annotation)) {}

    BitsAllClearMatchExpression(StringData path,
                                uint64_t bitMask,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ALL_CLEAR, path, bitMask, std::move(annotation)) {}

    BitsAllClearMatchExpression(StringData path,
                                const char* bitMaskBinary,
                                uint32_t bitMaskLen,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ALL_CLEAR, path, bitMaskBinary, bitMaskLen, std::move(annotation)) {}

    std::unique_ptr<MatchExpression> shallowClone() const final {
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
    BitsAnySetMatchExpression(StringData path,
                              std::vector<uint32_t> bitPositions,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ANY_SET, path, bitPositions, std::move(annotation)) {}

    BitsAnySetMatchExpression(StringData path,
                              uint64_t bitMask,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ANY_SET, path, bitMask, std::move(annotation)) {}

    BitsAnySetMatchExpression(StringData path,
                              const char* bitMaskBinary,
                              uint32_t bitMaskLen,
                              clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ANY_SET, path, bitMaskBinary, bitMaskLen, std::move(annotation)) {}

    std::unique_ptr<MatchExpression> shallowClone() const final {
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
    BitsAnyClearMatchExpression(StringData path,
                                std::vector<uint32_t> bitPositions,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ANY_CLEAR, path, bitPositions, std::move(annotation)) {}

    BitsAnyClearMatchExpression(StringData path,
                                uint64_t bitMask,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(BITS_ANY_CLEAR, path, bitMask, std::move(annotation)) {}

    BitsAnyClearMatchExpression(StringData path,
                                const char* bitMaskBinary,
                                uint32_t bitMaskLen,
                                clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : BitTestMatchExpression(
              BITS_ANY_CLEAR, path, bitMaskBinary, bitMaskLen, std::move(annotation)) {}

    std::unique_ptr<MatchExpression> shallowClone() const final {
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

/**
 * MatchExpression that represents a filter for a range of values. Similar semantics to the BETWEEN
 * SQL operator. Used for encoding a range query against an encrypted field.
 */
class EncryptedBetweenMatchExpression final : public LeafMatchExpression {
public:
    static constexpr StringData kName = "$encryptedBetween"_sd;

    EncryptedBetweenMatchExpression(StringData path,
                                    BSONElement rhs,
                                    clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : LeafMatchExpression(ENCRYPTED_BETWEEN, path, annotation) {
        _backingBSON = BSON("" << rhs);
    }


    BSONElement rhs() const {
        return _backingBSON.firstElement();
    }

    bool matchesSingleElement(const BSONElement& e, MatchDetails* details = nullptr) const final {
        // TODO: SERVER-67627 Implement runtime tag generation for $encryptedBetween.
        uasserted(6762800, "Not implemented");
    }

    BSONObj getSerializedRightHandSide() const final {
        return BSON(kName << _backingBSON.firstElement());
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        std::unique_ptr<EncryptedBetweenMatchExpression> e =
            std::make_unique<EncryptedBetweenMatchExpression>(path(), rhs(), _errorAnnotation);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return e;
    }

    virtual bool equivalent(const MatchExpression* other) const {
        if (matchType() != other->matchType()) {
            return false;
        }
        auto otherCast = static_cast<const EncryptedBetweenMatchExpression*>(other);
        return path() == otherCast->path() && rhs().binaryEqual(otherCast->rhs());
    };

    void debugString(StringBuilder& debug, int indentationLevel) const;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    BSONObj _backingBSON;

    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }
};

}  // namespace mongo
