/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_map.h"

namespace pcrecpp {
class RE;
}  // namespace pcrecpp;

namespace mongo {

class CollatorInterface;

class LeafMatchExpression : public PathMatchExpression {
public:
    LeafMatchExpression(MatchType matchType, StringData path)
        : LeafMatchExpression(matchType,
                              path,
                              ElementPath::LeafArrayBehavior::kTraverse,
                              ElementPath::NonLeafArrayBehavior::kTraverse) {}

    LeafMatchExpression(MatchType matchType,
                        StringData path,
                        ElementPath::LeafArrayBehavior leafArrBehavior,
                        ElementPath::NonLeafArrayBehavior nonLeafArrBehavior)
        : PathMatchExpression(matchType, path, leafArrBehavior, nonLeafArrBehavior) {}

    virtual ~LeafMatchExpression() = default;

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<MatchExpression*>* getChildVector() override {
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

    ComparisonMatchExpressionBase(MatchType type,
                                  StringData path,
                                  const BSONElement& rhs,
                                  ElementPath::LeafArrayBehavior,
                                  ElementPath::NonLeafArrayBehavior);

    virtual ~ComparisonMatchExpressionBase() = default;

    virtual void debugString(StringBuilder& debug, int level = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * Returns the name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    const BSONElement& getData() const {
        return _rhs;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

protected:
    /**
     * 'collator' must outlive the ComparisonMatchExpression and any clones made of it.
     */
    void _doSetCollator(const CollatorInterface* collator) final {
        _collator = collator;
    }

    BSONElement _rhs;

    // Collator used to compare elements. By default, simple binary comparison will be used.
    const CollatorInterface* _collator = nullptr;

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }
};

/**
 * EQ, LTE, LT, GT, GTE subclass from ComparisonMatchExpression.
 */
class ComparisonMatchExpression : public ComparisonMatchExpressionBase {
public:
    /**
     * Returns true if the MatchExpression is a ComparisonMatchExpression.
     */
    static bool isComparisonMatchExpression(const MatchExpression* expr) {
        switch (expr->matchType()) {
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

    ComparisonMatchExpression(MatchType type, StringData path, const BSONElement& rhs);

    virtual ~ComparisonMatchExpression() = default;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;
};

class EqualityMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$eq"_sd;

    EqualityMatchExpression(StringData path, const BSONElement& rhs)
        : ComparisonMatchExpression(EQ, path, rhs) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e =
            stdx::make_unique<EqualityMatchExpression>(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class LTEMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$lte"_sd;

    LTEMatchExpression(StringData path, const BSONElement& rhs)
        : ComparisonMatchExpression(LTE, path, rhs) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e =
            stdx::make_unique<LTEMatchExpression>(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class LTMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$lt"_sd;

    LTMatchExpression(StringData path, const BSONElement& rhs)
        : ComparisonMatchExpression(LT, path, rhs) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e =
            stdx::make_unique<LTMatchExpression>(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class GTMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$gt"_sd;

    GTMatchExpression(StringData path, const BSONElement& rhs)
        : ComparisonMatchExpression(GT, path, rhs) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e =
            stdx::make_unique<GTMatchExpression>(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class GTEMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$gte"_sd;

    GTEMatchExpression(StringData path, const BSONElement& rhs)
        : ComparisonMatchExpression(GTE, path, rhs) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e =
            stdx::make_unique<GTEMatchExpression>(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class RegexMatchExpression : public LeafMatchExpression {
public:
    /**
     * The maximum length of regex patterns.
     */
    static constexpr size_t kMaxPatternSize = 32764;

    RegexMatchExpression(StringData path, const BSONElement& e);
    RegexMatchExpression(StringData path, StringData regex, StringData options);

    ~RegexMatchExpression();

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<RegexMatchExpression> e =
            stdx::make_unique<RegexMatchExpression>(path(), _regex, _flags);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    void serializeToBSONTypeRegex(BSONObjBuilder* out) const;

    void shortDebugString(StringBuilder& debug) const;

    virtual bool equivalent(const MatchExpression* other) const;

    const std::string& getString() const {
        return _regex;
    }
    const std::string& getFlags() const {
        return _flags;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    void _init();

    std::string _regex;
    std::string _flags;
    std::unique_ptr<pcrecpp::RE> _re;
};

class ModMatchExpression : public LeafMatchExpression {
public:
    ModMatchExpression(StringData path, int divisor, int remainder);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ModMatchExpression> m =
            stdx::make_unique<ModMatchExpression>(path(), _divisor, _remainder);
        if (getTag()) {
            m->setTag(getTag()->clone());
        }
        return std::move(m);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    int getDivisor() const {
        return _divisor;
    }
    int getRemainder() const {
        return _remainder;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    int _divisor;
    int _remainder;
};

class ExistsMatchExpression : public LeafMatchExpression {
public:
    explicit ExistsMatchExpression(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ExistsMatchExpression> e = stdx::make_unique<ExistsMatchExpression>(path());
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

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
    explicit InMatchExpression(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * 'collator' must outlive the InMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator);

    Status setEqualities(std::vector<BSONElement> equalities);

    Status addRegex(std::unique_ptr<RegexMatchExpression> expr);

    const BSONEltFlatSet& getEqualities() const {
        return _equalitySet;
    }

    const std::vector<std::unique_ptr<RegexMatchExpression>>& getRegexes() const {
        return _regexes;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    bool hasNull() const {
        return _hasNull;
    }

    bool hasEmptyArray() const {
        return _hasEmptyArray;
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
    std::vector<BSONElement> _originalEqualityVector;

    // Set of equality elements associated with this expression. '_eltCmp' is used as a comparator
    // for this set.
    BSONEltFlatSet _equalitySet;

    // Container of regex elements this object owns.
    std::vector<std::unique_ptr<RegexMatchExpression>> _regexes;
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
                                    std::vector<uint32_t> bitPositions);
    explicit BitTestMatchExpression(MatchType type, StringData path, uint64_t bitMask);
    explicit BitTestMatchExpression(MatchType type,
                                    StringData path,
                                    const char* bitMaskBinary,
                                    uint32_t bitMaskLen);
    virtual ~BitTestMatchExpression() {}

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    size_t numBitPositions() const {
        return _bitPositions.size();
    }

    const std::vector<uint32_t>& getBitPositions() const {
        return _bitPositions;
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
};

class BitsAllSetMatchExpression : public BitTestMatchExpression {
public:
    BitsAllSetMatchExpression(StringData path, std::vector<uint32_t> bitPositions)
        : BitTestMatchExpression(BITS_ALL_SET, path, bitPositions) {}

    BitsAllSetMatchExpression(StringData path, uint64_t bitMask)
        : BitTestMatchExpression(BITS_ALL_SET, path, bitMask) {}

    BitsAllSetMatchExpression(StringData path, const char* bitMaskBinary, uint32_t bitMaskLen)
        : BitTestMatchExpression(BITS_ALL_SET, path, bitMaskBinary, bitMaskLen) {}

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAllSetMatchExpression>(path(), getBitPositions());
        if (getTag()) {
            bitTestMatchExpression->setTag(getTag()->clone());
        }
        return std::move(bitTestMatchExpression);
    }
};

class BitsAllClearMatchExpression : public BitTestMatchExpression {
public:
    BitsAllClearMatchExpression(StringData path, std::vector<uint32_t> bitPositions)
        : BitTestMatchExpression(BITS_ALL_CLEAR, path, bitPositions) {}

    BitsAllClearMatchExpression(StringData path, uint64_t bitMask)
        : BitTestMatchExpression(BITS_ALL_CLEAR, path, bitMask) {}

    BitsAllClearMatchExpression(StringData path, const char* bitMaskBinary, uint32_t bitMaskLen)
        : BitTestMatchExpression(BITS_ALL_CLEAR, path, bitMaskBinary, bitMaskLen) {}

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAllClearMatchExpression>(path(), getBitPositions());
        if (getTag()) {
            bitTestMatchExpression->setTag(getTag()->clone());
        }
        return std::move(bitTestMatchExpression);
    }
};

class BitsAnySetMatchExpression : public BitTestMatchExpression {
public:
    BitsAnySetMatchExpression(StringData path, std::vector<uint32_t> bitPositions)
        : BitTestMatchExpression(BITS_ANY_SET, path, bitPositions) {}

    BitsAnySetMatchExpression(StringData path, uint64_t bitMask)
        : BitTestMatchExpression(BITS_ANY_SET, path, bitMask) {}

    BitsAnySetMatchExpression(StringData path, const char* bitMaskBinary, uint32_t bitMaskLen)
        : BitTestMatchExpression(BITS_ANY_SET, path, bitMaskBinary, bitMaskLen) {}

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAnySetMatchExpression>(path(), getBitPositions());
        if (getTag()) {
            bitTestMatchExpression->setTag(getTag()->clone());
        }
        return std::move(bitTestMatchExpression);
    }
};

class BitsAnyClearMatchExpression : public BitTestMatchExpression {
public:
    BitsAnyClearMatchExpression(StringData path, std::vector<uint32_t> bitPositions)
        : BitTestMatchExpression(BITS_ANY_CLEAR, path, bitPositions) {}

    BitsAnyClearMatchExpression(StringData path, uint64_t bitMask)
        : BitTestMatchExpression(BITS_ANY_CLEAR, path, bitMask) {}

    BitsAnyClearMatchExpression(StringData path, const char* bitMaskBinary, uint32_t bitMaskLen)
        : BitTestMatchExpression(BITS_ANY_CLEAR, path, bitMaskBinary, bitMaskLen) {}

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAnyClearMatchExpression>(path(), getBitPositions());
        if (getTag()) {
            bitTestMatchExpression->setTag(getTag()->clone());
        }
        return std::move(bitTestMatchExpression);
    }
};

}  // namespace mongo
