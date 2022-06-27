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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression_leaf.h"

#include <cmath>
#include <memory>

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/config.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"

namespace mongo {

ComparisonMatchExpressionBase::ComparisonMatchExpressionBase(
    MatchType type,
    StringData path,
    Value rhs,
    ElementPath::LeafArrayBehavior leafArrBehavior,
    ElementPath::NonLeafArrayBehavior nonLeafArrBehavior,
    clonable_ptr<ErrorAnnotation> annotation,
    const CollatorInterface* collator)
    : LeafMatchExpression(type, path, leafArrBehavior, nonLeafArrBehavior, std::move(annotation)),
      _backingBSON(BSON(path << rhs)),
      _collator(collator) {
    setData(_backingBSON.firstElement());
    invariant(_rhs.type() != BSONType::EOO);
}

bool ComparisonMatchExpressionBase::equivalent(const MatchExpression* other) const {
    if (other->matchType() != matchType())
        return false;
    auto realOther = static_cast<const ComparisonMatchExpressionBase*>(other);

    if (!CollatorInterface::collatorsMatch(_collator, realOther->_collator)) {
        return false;
    }

    const StringData::ComparatorInterface* stringComparator = nullptr;
    BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore, stringComparator);
    return path() == realOther->path() && eltCmp.evaluate(_rhs == realOther->_rhs);
}

void ComparisonMatchExpressionBase::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " " << name();
    debug << " " << _rhs.toString(false);

    MatchExpression::TagData* td = getTag();
    if (td) {
        debug << " ";
        td->debugString(&debug);
    }

    debug << "\n";
}

BSONObj ComparisonMatchExpressionBase::getSerializedRightHandSide() const {
    return BSON(name() << _rhs);
}

ComparisonMatchExpression::ComparisonMatchExpression(MatchType type,
                                                     StringData path,
                                                     Value rhs,
                                                     clonable_ptr<ErrorAnnotation> annotation,
                                                     const CollatorInterface* collator)
    : ComparisonMatchExpressionBase(type,
                                    path,
                                    std::move(rhs),
                                    ElementPath::LeafArrayBehavior::kTraverse,
                                    ElementPath::NonLeafArrayBehavior::kTraverse,
                                    std::move(annotation),
                                    collator) {
    uassert(
        ErrorCodes::BadValue, "cannot compare to undefined", _rhs.type() != BSONType::Undefined);

    switch (matchType()) {
        case LT:
        case LTE:
        case EQ:
        case GT:
        case GTE:
            break;
        default:
            uasserted(ErrorCodes::BadValue, "bad match type for ComparisonMatchExpression");
    }
}

bool ComparisonMatchExpression::matchesSingleElement(const BSONElement& e,
                                                     MatchDetails* details) const {
    if (e.type() != _rhs.type()) {
        const auto ect = e.canonicalType();
        const auto rct = _rhs.canonicalType();
        if (ect != rct) {
            // We can't call 'compareElements' on elements of different canonical types.  Usually
            // elements with different canonical types should never match any comparison, but there
            // are a few exceptions, handled here.
            // jstNULL and undefined are treated the same
            if (ect + rct == 5) {
                return matchType() == EQ || matchType() == LTE || matchType() == GTE;
            }
            if (_rhs.type() == MaxKey || _rhs.type() == MinKey) {
                switch (matchType()) {
                    // LT and LTE need no distinction here because the two elements that we are
                    // comparing do not even have the same canonical type and are thus not equal
                    // (i.e.the case where we compare MinKey against MinKey would not reach this
                    // switch statement at all).  The same reasoning follows for the lack of
                    // distinction between GTE and GT.
                    case LT:
                    case LTE:
                        return _rhs.type() == MaxKey;
                    case EQ:
                        return false;
                    case GT:
                    case GTE:
                        return _rhs.type() == MinKey;
                    default:
                        // This is a comparison match expression, so it must be either
                        // a $lt, $lte, $gt, $gte, or equality expression.
                        MONGO_UNREACHABLE;
                }
            }
            return false;
        }
    }

    if (matchType() == EQ) {
        if (!_collator && e.type() == String) {
            // We know from above that _rhs must also be a String (or Symbol which has the same
            // representation) so if they have different value sizes, they must be different
            // strings. We can only stop here with the default collator, since other collators may
            // consider different length strings as equal.
            if (e.valuesize() != _rhs.valuesize())
                return false;
        }
    } else {
        // Special case handling for NaN. NaN is equal to NaN but otherwise always compares to
        // false. This follows the normal comparison rules (where NaN is less than all numbers) for
        // EQ, so we only need to do this for other comparison types.
        const bool lhsIsNan =  //
            (((e.type() == NumberDouble) && (std::isnan(e._numberDouble())))) ||
            ((e.type() == NumberDecimal) && (e._numberDecimal().isNaN()));
        const bool rhsIsNan =
            (((_rhs.type() == NumberDouble) && (std::isnan(_rhs._numberDouble()))) ||
             ((_rhs.type() == NumberDecimal) && (_rhs._numberDecimal().isNaN())));
        if (lhsIsNan || rhsIsNan) {
            bool bothNaN = lhsIsNan && rhsIsNan;
            switch (matchType()) {
                case LT:
                case GT:
                    return false;
                case LTE:
                case GTE:
                    return bothNaN;
                default:
                    // This is a comparison match expression, so it must be either
                    // a $lt, $lte, $gt, $gte, or equality expression.
                    fassertFailed(17448);
            }
        }
    }

    int x = BSONElement::compareElements(
        e, _rhs, BSONElement::ComparisonRules::kConsiderFieldName, _collator);
    switch (matchType()) {
        case LT:
            return x < 0;
        case LTE:
            return x <= 0;
        case EQ:
            return x == 0;
        case GT:
            return x > 0;
        case GTE:
            return x >= 0;
        default:
            // This is a comparison match expression, so it must be either
            // a $lt, $lte, $gt, $gte, or equality expression.
            fassertFailed(16828);
    }
}

constexpr StringData EqualityMatchExpression::kName;
constexpr StringData LTMatchExpression::kName;
constexpr StringData LTEMatchExpression::kName;
constexpr StringData GTMatchExpression::kName;
constexpr StringData GTEMatchExpression::kName;

const std::set<char> RegexMatchExpression::kValidRegexFlags = {'i', 'm', 's', 'x'};

std::unique_ptr<pcre::Regex> RegexMatchExpression::makeRegex(const std::string& regex,
                                                             const std::string& flags) {
    return std::make_unique<pcre::Regex>(regex, pcre_util::flagsToOptions(flags));
}

RegexMatchExpression::RegexMatchExpression(StringData path,
                                           StringData regex,
                                           StringData options,
                                           clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(REGEX, path, std::move(annotation)),
      _regex(regex.toString()),
      _flags(options.toString()),
      _re(makeRegex(_regex, _flags)) {

    uassert(ErrorCodes::BadValue,
            "Regular expression cannot contain an embedded null byte",
            _regex.find('\0') == std::string::npos);

    uassert(51091,
            str::stream() << "Regular expression is invalid: " << errorMessage(_re->error()),
            *_re);
}

RegexMatchExpression::~RegexMatchExpression() {}

bool RegexMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const RegexMatchExpression* realOther = static_cast<const RegexMatchExpression*>(other);
    return path() == realOther->path() && _regex == realOther->_regex &&
        _flags == realOther->_flags;
}

bool RegexMatchExpression::matchesSingleElement(const BSONElement& e, MatchDetails* details) const {
    switch (e.type()) {
        case String:
        case Symbol:
            return !!_re->matchView(e.valueStringData());
        case RegEx:
            return _regex == e.regex() && _flags == e.regexFlags();
        default:
            return false;
    }
}

void RegexMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " regex /" << _regex << "/" << _flags;

    MatchExpression::TagData* td = getTag();
    if (nullptr != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

BSONObj RegexMatchExpression::getSerializedRightHandSide() const {
    BSONObjBuilder regexBuilder;
    regexBuilder.append("$regex", _regex);

    if (!_flags.empty()) {
        regexBuilder.append("$options", _flags);
    }

    return regexBuilder.obj();
}

void RegexMatchExpression::serializeToBSONTypeRegex(BSONObjBuilder* out) const {
    out->appendRegex(path(), _regex, _flags);
}

void RegexMatchExpression::shortDebugString(StringBuilder& debug) const {
    debug << "/" << _regex << "/" << _flags;
}

// ---------

ModMatchExpression::ModMatchExpression(StringData path,
                                       long long divisor,
                                       long long remainder,
                                       clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(MOD, path, std::move(annotation)),
      _divisor(divisor),
      _remainder(remainder) {
    uassert(ErrorCodes::BadValue, "divisor cannot be 0", divisor != 0);
}

bool ModMatchExpression::matchesSingleElement(const BSONElement& e, MatchDetails* details) const {
    if (!e.isNumber())
        return false;
    long long dividend;
    if (e.type() == BSONType::NumberDouble) {
        auto dividendDouble = e.Double();

        // If dividend is NaN or Infinity, then there is no match.
        if (!std::isfinite(dividendDouble)) {
            return false;
        }
        auto dividendLong = representAs<long long>(std::trunc(dividendDouble));

        // If the dividend value cannot be represented as a 64-bit integer, then we return false.
        if (!dividendLong) {
            return false;
        }
        dividend = *dividendLong;
    } else if (e.type() == BSONType::NumberDecimal) {
        auto dividendDecimal = e.Decimal();

        // If dividend is NaN or Infinity, then there is no match.
        if (!dividendDecimal.isFinite()) {
            return false;
        }
        auto dividendLong =
            representAs<long long>(dividendDecimal.round(Decimal128::kRoundTowardZero));

        // If the dividend value cannot be represented as a 64-bit integer, then we return false.
        if (!dividendLong) {
            return false;
        }
        dividend = *dividendLong;
    } else {
        dividend = e.numberLong();
    }
    return overflow::safeMod(dividend, _divisor) == _remainder;
}

void ModMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " mod " << _divisor << " % x == " << _remainder;
    MatchExpression::TagData* td = getTag();
    if (nullptr != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

BSONObj ModMatchExpression::getSerializedRightHandSide() const {
    return BSON("$mod" << BSON_ARRAY(_divisor << _remainder));
}

bool ModMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ModMatchExpression* realOther = static_cast<const ModMatchExpression*>(other);
    return path() == realOther->path() && _divisor == realOther->_divisor &&
        _remainder == realOther->_remainder;
}


// ------------------

ExistsMatchExpression::ExistsMatchExpression(StringData path,
                                             clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(EXISTS, path, std::move(annotation)) {}

bool ExistsMatchExpression::matchesSingleElement(const BSONElement& e,
                                                 MatchDetails* details) const {
    return !e.eoo();
}

void ExistsMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " exists";
    MatchExpression::TagData* td = getTag();
    if (nullptr != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

BSONObj ExistsMatchExpression::getSerializedRightHandSide() const {
    return BSON("$exists" << true);
}

bool ExistsMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ExistsMatchExpression* realOther = static_cast<const ExistsMatchExpression*>(other);
    return path() == realOther->path();
}


// ----

InMatchExpression::InMatchExpression(StringData path, clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(MATCH_IN, path, std::move(annotation)),
      _eltCmp(BSONElementComparator::FieldNamesMode::kIgnore, _collator) {}

std::unique_ptr<MatchExpression> InMatchExpression::shallowClone() const {
    auto next = std::make_unique<InMatchExpression>(path(), _errorAnnotation);
    next->setCollator(_collator);
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    next->_hasNull = _hasNull;
    next->_hasEmptyArray = _hasEmptyArray;
    next->_equalitySet = _equalitySet;
    next->_originalEqualityVector = _originalEqualityVector;
    next->_equalityStorage = _equalityStorage;
    for (auto&& regex : _regexes) {
        std::unique_ptr<RegexMatchExpression> clonedRegex(
            static_cast<RegexMatchExpression*>(regex->shallowClone().release()));
        next->_regexes.push_back(std::move(clonedRegex));
    }
    if (getInputParamId()) {
        next->setInputParamId(*getInputParamId());
    }
    return next;
}

bool InMatchExpression::contains(const BSONElement& e) const {
    return std::binary_search(_equalitySet.begin(), _equalitySet.end(), e, _eltCmp.makeLessThan());
}

bool InMatchExpression::matchesSingleElement(const BSONElement& e, MatchDetails* details) const {
    // When an $in has a null, it adopts the same semantics as {$eq:null}. Namely, in addition to
    // matching literal null values, the $in should match missing and undefined.
    if (_hasNull && (e.eoo() || e.type() == BSONType::Undefined)) {
        return true;
    }
    if (contains(e)) {
        return true;
    }
    for (auto&& regex : _regexes) {
        if (regex->matchesSingleElement(e, details)) {
            return true;
        }
    }
    return false;
}

void InMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " $in ";
    debug << "[ ";
    for (auto&& equality : _equalitySet) {
        debug << equality.toString(false) << " ";
    }
    for (auto&& regex : _regexes) {
        regex->shortDebugString(debug);
        debug << " ";
    }
    debug << "]";
    MatchExpression::TagData* td = getTag();
    if (nullptr != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

BSONObj InMatchExpression::getSerializedRightHandSide() const {
    BSONObjBuilder inBob;
    BSONArrayBuilder arrBob(inBob.subarrayStart("$in"));
    for (auto&& _equality : _equalitySet) {
        arrBob.append(_equality);
    }
    for (auto&& _regex : _regexes) {
        BSONObjBuilder regexBob;
        _regex->serializeToBSONTypeRegex(&regexBob);
        arrBob.append(regexBob.obj().firstElement());
    }
    arrBob.doneFast();
    return inBob.obj();
}

bool InMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }
    const InMatchExpression* realOther = static_cast<const InMatchExpression*>(other);
    if (path() != realOther->path()) {
        return false;
    }
    if (_hasNull != realOther->_hasNull) {
        return false;
    }
    if (_regexes.size() != realOther->_regexes.size()) {
        return false;
    }
    for (size_t i = 0; i < _regexes.size(); ++i) {
        if (!_regexes[i]->equivalent(realOther->_regexes[i].get())) {
            return false;
        }
    }
    if (!CollatorInterface::collatorsMatch(_collator, realOther->_collator)) {
        return false;
    }
    // We use an element-wise comparison to check equivalence of '_equalitySet'.  Unfortunately, we
    // can't use BSONElementSet::operator==(), as it does not use the comparator object the set is
    // initialized with (and as such, it is not collation-aware).
    if (_equalitySet.size() != realOther->_equalitySet.size()) {
        return false;
    }
    auto thisEqIt = _equalitySet.begin();
    auto otherEqIt = realOther->_equalitySet.begin();
    for (; thisEqIt != _equalitySet.end(); ++thisEqIt, ++otherEqIt) {
        const bool considerFieldName = false;
        if (thisEqIt->woCompare(*otherEqIt, considerFieldName, _collator)) {
            return false;
        }
    }
    invariant(otherEqIt == realOther->_equalitySet.end());
    return true;
}

void InMatchExpression::_doSetCollator(const CollatorInterface* collator) {
    _collator = collator;
    _eltCmp = BSONElementComparator(BSONElementComparator::FieldNamesMode::kIgnore, _collator);

    if (!std::is_sorted(_originalEqualityVector.begin(),
                        _originalEqualityVector.end(),
                        _eltCmp.makeLessThan())) {
        std::sort(
            _originalEqualityVector.begin(), _originalEqualityVector.end(), _eltCmp.makeLessThan());
    }

    // We need to re-compute '_equalitySet', since our set comparator has changed.
    _equalitySet.clear();
    _equalitySet.reserve(_originalEqualityVector.size());
    std::unique_copy(_originalEqualityVector.begin(),
                     _originalEqualityVector.end(),
                     std::back_inserter(_equalitySet),
                     _eltCmp.makeEqualTo());
}

Status InMatchExpression::setEqualities(std::vector<BSONElement> equalities) {
    for (auto&& equality : equalities) {
        if (equality.type() == BSONType::RegEx) {
            return Status(ErrorCodes::BadValue, "InMatchExpression equality cannot be a regex");
        }
        if (equality.type() == BSONType::Undefined) {
            return Status(ErrorCodes::BadValue, "InMatchExpression equality cannot be undefined");
        }

        if (equality.type() == BSONType::jstNULL) {
            _hasNull = true;
        } else if (equality.type() == BSONType::Array && equality.Obj().isEmpty()) {
            _hasEmptyArray = true;
        }
    }

    _originalEqualityVector = std::move(equalities);

    if (!std::is_sorted(_originalEqualityVector.begin(),
                        _originalEqualityVector.end(),
                        _eltCmp.makeLessThan())) {
        std::sort(
            _originalEqualityVector.begin(), _originalEqualityVector.end(), _eltCmp.makeLessThan());
    }

    _equalitySet.clear();
    _equalitySet.reserve(_originalEqualityVector.size());
    std::unique_copy(_originalEqualityVector.begin(),
                     _originalEqualityVector.end(),
                     std::back_inserter(_equalitySet),
                     _eltCmp.makeEqualTo());

    return Status::OK();
}

void InMatchExpression::setBackingBSON(BSONObj equalityStorage) {
    _equalityStorage = std::move(equalityStorage);
}

Status InMatchExpression::addRegex(std::unique_ptr<RegexMatchExpression> expr) {
    _regexes.push_back(std::move(expr));
    return Status::OK();
}

MatchExpression::ExpressionOptimizerFunc InMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) -> std::unique_ptr<MatchExpression> {
        // NOTE: We do not recursively call optimize() on the RegexMatchExpression children in the
        // _regexes list. We assume that optimize() on a RegexMatchExpression is a no-op.

        auto& regexList = static_cast<InMatchExpression&>(*expression)._regexes;
        auto& equalitySet = static_cast<InMatchExpression&>(*expression)._equalitySet;
        auto collator = static_cast<InMatchExpression&>(*expression).getCollator();
        if (regexList.size() == 1 && equalitySet.empty()) {
            // Simplify IN of exactly one regex to be a regex match.
            auto& childRe = regexList.front();
            invariant(!childRe->getTag());

            auto simplifiedExpression = std::make_unique<RegexMatchExpression>(
                expression->path(), childRe->getString(), childRe->getFlags());
            if (expression->getTag()) {
                simplifiedExpression->setTag(expression->getTag()->clone());
            }
            return simplifiedExpression;
        } else if (equalitySet.size() == 1 && regexList.empty()) {
            // Simplify IN of exactly one equality to be an EqualityMatchExpression.
            auto simplifiedExpression = std::make_unique<EqualityMatchExpression>(
                expression->path(), *(equalitySet.begin()));
            simplifiedExpression->setCollator(collator);
            if (expression->getTag()) {
                simplifiedExpression->setTag(expression->getTag()->clone());
            }

            return simplifiedExpression;
        }

        return expression;
    };
}

// -----------

BitTestMatchExpression::BitTestMatchExpression(MatchType type,
                                               StringData path,
                                               std::vector<uint32_t> bitPositions,
                                               clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(type, path, std::move(annotation)),
      _bitPositions(std::move(bitPositions)) {
    // Process bit positions into bitmask.
    for (auto bitPosition : _bitPositions) {
        // Checking bits > 63 is just checking the sign bit, since we sign-extend numbers. For
        // example, the 100th bit of -1 is considered set if and only if the 63rd bit position is
        // set.
        bitPosition = std::min(bitPosition, 63U);
        _bitMask |= 1ULL << bitPosition;
    }
}

BitTestMatchExpression::BitTestMatchExpression(MatchType type,
                                               StringData path,
                                               uint64_t bitMask,
                                               clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(type, path, std::move(annotation)), _bitMask(bitMask) {
    // Process bitmask into bit positions.
    for (int bit = 0; bit < 64; bit++) {
        if (_bitMask & (1ULL << bit)) {
            _bitPositions.push_back(bit);
        }
    }
}

BitTestMatchExpression::BitTestMatchExpression(MatchType type,
                                               StringData path,
                                               const char* bitMaskBinary,
                                               uint32_t bitMaskLen,
                                               clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(type, path, std::move(annotation)) {
    for (uint32_t byte = 0; byte < bitMaskLen; byte++) {
        char byteAt = bitMaskBinary[byte];
        if (!byteAt) {
            continue;
        }

        // Build _bitMask with the first 8 bytes of the bitMaskBinary.
        if (byte < 8) {
            _bitMask |= static_cast<uint64_t>(byteAt) << byte * 8;
        } else {
            // Checking bits > 63 is just checking the sign bit, since we sign-extend numbers. For
            // example, the 100th bit of -1 is considered set if and only if the 63rd bit position
            // is set.
            _bitMask |= 1ULL << 63;
        }

        for (int bit = 0; bit < 8; bit++) {
            if (byteAt & (1 << bit)) {
                _bitPositions.push_back(8 * byte + bit);
            }
        }
    }
}

bool BitTestMatchExpression::needFurtherBitTests(bool isBitSet) const {
    const MatchType mt = matchType();

    return (isBitSet && (mt == BITS_ALL_SET || mt == BITS_ANY_CLEAR)) ||
        (!isBitSet && (mt == BITS_ALL_CLEAR || mt == BITS_ANY_SET));
}

bool BitTestMatchExpression::performBitTest(long long eValue) const {
    const MatchType mt = matchType();

    switch (mt) {
        case BITS_ALL_SET:
            return (eValue & _bitMask) == _bitMask;
        case BITS_ALL_CLEAR:
            return (~eValue & _bitMask) == _bitMask;
        case BITS_ANY_SET:
            return eValue & _bitMask;
        case BITS_ANY_CLEAR:
            return ~eValue & _bitMask;
        default:
            MONGO_UNREACHABLE;
    }
}

bool BitTestMatchExpression::performBitTest(const char* eBinary, uint32_t eBinaryLen) const {
    const MatchType mt = matchType();

    // Test each bit position.
    for (auto bitPosition : _bitPositions) {
        bool isBitSet;
        if (bitPosition >= eBinaryLen * 8) {
            // If position to test is longer than the data to test against, zero-extend.
            isBitSet = false;
        } else {
            // Map to byte position and bit position within that byte. Note that byte positions
            // start at position 0 in the char array, and bit positions start at the least
            // significant bit.
            int bytePosition = bitPosition / 8;
            int bit = bitPosition % 8;
            char byte = eBinary[bytePosition];

            isBitSet = byte & (1 << bit);
        }

        if (!needFurtherBitTests(isBitSet)) {
            // If we can skip the rest fo the tests, that means we succeeded with _ANY_ or failed
            // with _ALL_.
            return mt == BITS_ANY_SET || mt == BITS_ANY_CLEAR;
        }
    }

    // If we finished all the tests, that means we succeeded with _ALL_ or failed with _ANY_.
    return mt == BITS_ALL_SET || mt == BITS_ALL_CLEAR;
}

bool BitTestMatchExpression::matchesSingleElement(const BSONElement& e,
                                                  MatchDetails* details) const {
    // Validate 'e' is a number or a BinData.
    if (!e.isNumber() && e.type() != BSONType::BinData) {
        return false;
    }

    if (e.type() == BSONType::BinData) {
        int eBinaryLen;  // Length of eBinary (in bytes).
        const char* eBinary = e.binData(eBinaryLen);
        return performBitTest(eBinary, eBinaryLen);
    }

    invariant(e.isNumber());

    if (e.type() == BSONType::NumberDouble) {
        double eDouble = e.numberDouble();

        // NaN doubles are rejected.
        if (std::isnan(eDouble)) {
            return false;
        }

        // Integral doubles that are too large or small to be represented as a 64-bit signed
        // integer are treated as 0. We use 'kLongLongMaxAsDouble' because if we just did
        // eDouble > 2^63-1, it would be compared against 2^63. eDouble=2^63 would not get caught
        // that way.
        if (eDouble >= BSONElement::kLongLongMaxPlusOneAsDouble ||
            eDouble < std::numeric_limits<long long>::min()) {
            return false;
        }

        // This checks if e is an integral double.
        if (eDouble != static_cast<double>(static_cast<long long>(eDouble))) {
            return false;
        }
    }

    long long eValue = e.numberLong();
    return performBitTest(eValue);
}

std::string BitTestMatchExpression::name() const {
    switch (matchType()) {
        case BITS_ALL_SET:
            return "$bitsAllSet";

        case BITS_ALL_CLEAR:
            return "$bitsAllClear";

        case BITS_ANY_SET:
            return "$bitsAnySet";

        case BITS_ANY_CLEAR:
            return "$bitsAnyClear";

        default:
            MONGO_UNREACHABLE;
    }
}

void BitTestMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    debug << path() << " " << name() << ": [";
    for (size_t i = 0; i < _bitPositions.size(); i++) {
        debug << _bitPositions[i];
        if (i != _bitPositions.size() - 1) {
            debug << ", ";
        }
    }
    debug << "]";

    MatchExpression::TagData* td = getTag();
    if (td) {
        debug << " ";
        td->debugString(&debug);
    }
}

BSONObj BitTestMatchExpression::getSerializedRightHandSide() const {
    std::string opString = "";

    switch (matchType()) {
        case BITS_ALL_SET:
            opString = "$bitsAllSet";
            break;
        case BITS_ALL_CLEAR:
            opString = "$bitsAllClear";
            break;
        case BITS_ANY_SET:
            opString = "$bitsAnySet";
            break;
        case BITS_ANY_CLEAR:
            opString = "$bitsAnyClear";
            break;
        default:
            MONGO_UNREACHABLE;
    }

    BSONArrayBuilder arrBob;
    for (auto bitPosition : _bitPositions) {
        arrBob.append(static_cast<int32_t>(bitPosition));
    }
    arrBob.doneFast();

    return BSON(opString << arrBob.arr());
}

bool BitTestMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }

    const BitTestMatchExpression* realOther = static_cast<const BitTestMatchExpression*>(other);

    std::vector<uint32_t> myBitPositions = getBitPositions();
    std::vector<uint32_t> otherBitPositions = realOther->getBitPositions();
    std::sort(myBitPositions.begin(), myBitPositions.end());
    std::sort(otherBitPositions.begin(), otherBitPositions.end());

    return path() == realOther->path() && myBitPositions == otherBitPositions;
}
}  // namespace mongo
