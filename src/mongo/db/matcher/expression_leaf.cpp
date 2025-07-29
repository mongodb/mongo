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

#include <boost/move/utility_core.hpp>
#include <boost/numeric/conversion/converter_policies.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace mongo {

template <typename T>
ComparisonMatchExpressionBase::ComparisonMatchExpressionBase(
    MatchType type,
    boost::optional<StringData> path,
    T&& rhs,
    ElementPath::LeafArrayBehavior leafArrBehavior,
    ElementPath::NonLeafArrayBehavior nonLeafArrBehavior,
    clonable_ptr<ErrorAnnotation> annotation,
    const CollatorInterface* collator)
    : LeafMatchExpression(type, path, leafArrBehavior, nonLeafArrBehavior, std::move(annotation)),
      _backingBSONIsSet(false),
      _collator(collator) {
    setData(path, std::move(rhs));
    invariant(!_rhs.eoo());
}

// Instantiate above constructor for 'Value&&' and 'const BSONElement&' types.
template ComparisonMatchExpressionBase::ComparisonMatchExpressionBase(
    MatchType,
    boost::optional<StringData>,
    Value&&,
    ElementPath::LeafArrayBehavior,
    ElementPath::NonLeafArrayBehavior,
    clonable_ptr<ErrorAnnotation>,
    const CollatorInterface*);
template ComparisonMatchExpressionBase::ComparisonMatchExpressionBase(
    MatchType,
    boost::optional<StringData>,
    const BSONElement&,
    ElementPath::LeafArrayBehavior,
    ElementPath::NonLeafArrayBehavior,
    clonable_ptr<ErrorAnnotation>,
    const CollatorInterface*);

bool ComparisonMatchExpressionBase::equivalent(const MatchExpression* other) const {
    if (other->matchType() != matchType())
        return false;
    auto realOther = static_cast<const ComparisonMatchExpressionBase*>(other);

    if (!CollatorInterface::collatorsMatch(_collator, realOther->_collator)) {
        return false;
    }

    // Please, keep BSONElementComparator consistent with MatchExpressionHasher defined in
    // db/matcher/expression_hasher.cpp.
    BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore, _collator);
    return path() == realOther->path() && eltCmp.evaluate(_rhs == realOther->_rhs);
}

void ComparisonMatchExpressionBase::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " " << name();
    debug << " " << _rhs.toString(false);
    _debugStringAttachTagInfo(&debug);
}

void ComparisonMatchExpressionBase::appendSerializedRightHandSide(BSONObjBuilder* bob,
                                                                  const SerializationOptions& opts,
                                                                  bool includePath) const {
    opts.appendLiteral(bob, name(), _rhs);
}

template <typename T>
ComparisonMatchExpression::ComparisonMatchExpression(MatchType type,
                                                     boost::optional<StringData> path,
                                                     T&& rhs,
                                                     clonable_ptr<ErrorAnnotation> annotation,
                                                     const CollatorInterface* collator)
    : ComparisonMatchExpressionBase(type,
                                    path,
                                    std::forward<T>(rhs),
                                    ElementPath::LeafArrayBehavior::kTraverse,
                                    ElementPath::NonLeafArrayBehavior::kTraverse,
                                    std::move(annotation),
                                    collator) {
    uassert(
        ErrorCodes::BadValue, "cannot compare to undefined", _rhs.type() != BSONType::undefined);

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

// Instantiate above constructor for 'Value&&' and 'const BSONElement&' types.
template ComparisonMatchExpression::ComparisonMatchExpression(MatchType,
                                                              boost::optional<StringData>,
                                                              Value&&,
                                                              clonable_ptr<ErrorAnnotation>,
                                                              const CollatorInterface*);
template ComparisonMatchExpression::ComparisonMatchExpression(MatchType,
                                                              boost::optional<StringData>,
                                                              const BSONElement&,
                                                              clonable_ptr<ErrorAnnotation>,
                                                              const CollatorInterface*);

constexpr StringData EqualityMatchExpression::kName;
constexpr StringData LTMatchExpression::kName;
constexpr StringData LTEMatchExpression::kName;
constexpr StringData GTMatchExpression::kName;
constexpr StringData GTEMatchExpression::kName;

const std::set<char> RegexMatchExpression::kValidRegexFlags = {'i', 'm', 's', 'x'};

std::unique_ptr<pcre::Regex> RegexMatchExpression::makeRegex(const std::string& regex,
                                                             const std::string& flags) {
    return std::make_unique<pcre::Regex>(
        regex,
        pcre_util::flagsToOptions(flags),
        pcre::Limits{
            .heapLimitKB = static_cast<uint32_t>(internalQueryRegexHeapLimitKB.loadRelaxed()),
            .matchLimit = static_cast<uint32_t>(internalQueryRegexMatchLimit.loadRelaxed())});
}

RegexMatchExpression::RegexMatchExpression(boost::optional<StringData> path,
                                           StringData regex,
                                           StringData options,
                                           clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(REGEX, path, std::move(annotation)),
      _regex(std::string{regex}),
      _flags(std::string{options}),
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

void RegexMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " regex /" << _regex << "/" << _flags;
    _debugStringAttachTagInfo(&debug);
}

void RegexMatchExpression::appendSerializedRightHandSide(BSONObjBuilder* bob,
                                                         const SerializationOptions& opts,
                                                         bool includePath) const {
    // We need to be careful to generate a valid regex representative value, and the default string
    // "?" is not valid.
    opts.appendLiteral(bob, "$regex", _regex, Value("\\?"_sd));

    if (!_flags.empty()) {
        // We need to make sure the $options value can be re-parsed as legal regex options, so
        // we'll set the representative value in this case to be the string "i" rather than
        // "?", which is the standard representative for string values.
        opts.appendLiteral(bob, "$options", _flags, Value("i"_sd));
    }
}

void RegexMatchExpression::serializeToBSONTypeRegex(BSONObjBuilder* out) const {
    out->appendRegex(path(), _regex, _flags);
}

void RegexMatchExpression::shortDebugString(StringBuilder& debug) const {
    debug << "/" << _regex << "/" << _flags;
}

// ---------

ModMatchExpression::ModMatchExpression(boost::optional<StringData> path,
                                       long long divisor,
                                       long long remainder,
                                       clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(MOD, path, std::move(annotation)),
      _divisor(divisor),
      _remainder(remainder) {
    uassert(ErrorCodes::BadValue, "divisor cannot be 0", divisor != 0);
}

void ModMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " mod " << _divisor << " % x == " << _remainder;
    _debugStringAttachTagInfo(&debug);
}

void ModMatchExpression::appendSerializedRightHandSide(BSONObjBuilder* bob,
                                                       const SerializationOptions& opts,
                                                       bool includePath) const {
    bob->append("$mod",
                BSON_ARRAY(opts.serializeLiteral(_divisor) << opts.serializeLiteral(_remainder)));
}

bool ModMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ModMatchExpression* realOther = static_cast<const ModMatchExpression*>(other);
    return path() == realOther->path() && _divisor == realOther->_divisor &&
        _remainder == realOther->_remainder;
}


// ------------------

ExistsMatchExpression::ExistsMatchExpression(boost::optional<StringData> path,
                                             clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(EXISTS, path, std::move(annotation)) {}

void ExistsMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " exists";
    _debugStringAttachTagInfo(&debug);
}

void ExistsMatchExpression::appendSerializedRightHandSide(BSONObjBuilder* bob,
                                                          const SerializationOptions& opts,
                                                          bool includePath) const {
    opts.appendLiteral(bob, "$exists", true);
}

bool ExistsMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ExistsMatchExpression* realOther = static_cast<const ExistsMatchExpression*>(other);
    return path() == realOther->path();
}


// ----

InMatchExpression::InMatchExpression(boost::optional<StringData> path,
                                     clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(MATCH_IN, path, std::move(annotation)),
      _equalities(std::make_shared<InListData>()) {}

InMatchExpression::InMatchExpression(boost::optional<StringData> path,
                                     clonable_ptr<ErrorAnnotation> annotation,
                                     std::shared_ptr<InListData> equalities)
    : LeafMatchExpression(MATCH_IN, path, std::move(annotation)),
      _equalities(std::move(equalities)) {}

std::unique_ptr<MatchExpression> InMatchExpression::clone() const {
    auto ime = std::make_unique<InMatchExpression>(path(), _errorAnnotation, _equalities->clone());

    if (getTag()) {
        ime->setTag(getTag()->clone());
    }

    for (auto&& regex : _regexes) {
        std::unique_ptr<RegexMatchExpression> clonedRegex(
            static_cast<RegexMatchExpression*>(regex->clone().release()));
        ime->_regexes.push_back(std::move(clonedRegex));
    }

    if (getInputParamId()) {
        ime->setInputParamId(*getInputParamId());
    }

    return ime;
}

void InMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " $in ";
    debug << "[ ";

    _equalities->writeToStream(debug);

    for (auto&& regex : _regexes) {
        regex->shortDebugString(debug);
        debug << " ";
    }
    debug << "]";

    _debugStringAttachTagInfo(&debug);
}

void InMatchExpression::serializeToShape(BSONObjBuilder* bob,
                                         const SerializationOptions& opts) const {
    auto firstElementOfEachType =
        _equalities->getFirstOfEachType(opts.inMatchExprSortAndDedupElements);

    std::vector<Value> firstOfEachType;
    firstOfEachType.reserve(firstElementOfEachType.size());
    for (auto&& elem : firstElementOfEachType) {
        firstOfEachType.emplace_back(elem);
    }

    if (hasRegex()) {
        firstOfEachType.emplace_back(BSONRegEx());
    }

    opts.appendLiteral(bob, "$in", std::move(firstOfEachType));
}

void InMatchExpression::appendSerializedRightHandSide(BSONObjBuilder* bob,
                                                      const SerializationOptions& opts,
                                                      bool includePath) const {
    if (!opts.isKeepingLiteralsUnchanged()) {
        serializeToShape(bob, opts);
        return;
    }

    BSONArrayBuilder arrBob(bob->subarrayStart("$in"));

    _equalities->appendElements(arrBob, opts.inMatchExprSortAndDedupElements);

    for (auto&& regex : _regexes) {
        BSONObjBuilder regexBob;
        regex->serializeToBSONTypeRegex(&regexBob);
        arrBob.append(regexBob.obj().firstElement());
    }

    arrBob.doneFast();
}

bool InMatchExpression::equivalent(const MatchExpression* other) const {
    constexpr BSONObj::ComparisonRulesSet kIgnoreFieldName = 0;

    if (matchType() != other->matchType()) {
        return false;
    }

    const InMatchExpression* ime = static_cast<const InMatchExpression*>(other);
    if (path() != ime->path() || _regexes.size() != ime->_regexes.size()) {
        return false;
    }

    if (_equalities->getTypeMask() != ime->_equalities->getTypeMask() ||
        !CollatorInterface::collatorsMatch(_equalities->getCollator(),
                                           ime->_equalities->getCollator())) {
        return false;
    }

    const auto& elems = _equalities->getElements();
    const auto& otherElems = ime->_equalities->getElements();
    if (elems.size() != otherElems.size()) {
        return false;
    }

    auto coll = _equalities->getCollator();
    auto thisEqIt = elems.begin();
    auto thisEqEndIt = elems.end();
    auto otherEqIt = otherElems.begin();
    for (; thisEqIt != thisEqEndIt; ++thisEqIt, ++otherEqIt) {
        if (thisEqIt->woCompare(*otherEqIt, kIgnoreFieldName, coll)) {
            return false;
        }
    }

    for (size_t i = 0; i < _regexes.size(); ++i) {
        if (!_regexes[i]->equivalent(ime->_regexes[i].get())) {
            return false;
        }
    }

    return true;
}

void InMatchExpression::_doSetCollator(const CollatorInterface* collator) {
    cloneEqualitiesBeforeWriteIfNeeded();
    _equalities->setCollator(collator);
}

Status InMatchExpression::addRegex(std::unique_ptr<RegexMatchExpression> expr) {
    _regexes.push_back(std::move(expr));
    return Status::OK();
}

// -----------

BitTestMatchExpression::BitTestMatchExpression(MatchType type,
                                               boost::optional<StringData> path,
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
                                               boost::optional<StringData> path,
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
                                               boost::optional<StringData> path,
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

    _debugStringAttachTagInfo(&debug);
}

void BitTestMatchExpression::appendSerializedRightHandSide(BSONObjBuilder* bob,
                                                           const SerializationOptions& opts,
                                                           bool includePath) const {
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
    // Unfortunately this cannot be done without copying the array into the BSONObjBuilder, since
    // `opts.appendLiteral` may choose to append this actual array, a representative empty array, or
    // a debug string.
    opts.appendLiteral(bob, opString, arrBob.arr());
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
