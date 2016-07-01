// expression_leaf.cpp

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

#include "mongo/db/matcher/expression_leaf.h"

#include <cmath>
#include <pcrecpp.h>
#include <unordered_map>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/config.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

Status LeafMatchExpression::setPath(StringData path) {
    _path = path;
    return _elementPath.init(_path);
}


bool LeafMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    MatchableDocument::IteratorHolder cursor(doc, &_elementPath);
    while (cursor->more()) {
        ElementIterator::Context e = cursor->next();
        if (!matchesSingleElement(e.element()))
            continue;
        if (details && details->needRecord() && !e.arrayOffset().eoo()) {
            details->setElemMatchKey(e.arrayOffset().fieldName());
        }
        return true;
    }
    return false;
}

// -------------

bool ComparisonMatchExpression::equivalent(const MatchExpression* other) const {
    if (other->matchType() != matchType())
        return false;
    const ComparisonMatchExpression* realOther =
        static_cast<const ComparisonMatchExpression*>(other);

    if (!CollatorInterface::collatorsMatch(_collator, realOther->_collator)) {
        return false;
    }

    return path() == realOther->path() && _rhs.valuesEqual(realOther->_rhs);
}


Status ComparisonMatchExpression::init(StringData path, const BSONElement& rhs) {
    _rhs = rhs;

    if (rhs.eoo()) {
        return Status(ErrorCodes::BadValue, "need a real operand");
    }

    if (rhs.type() == Undefined) {
        return Status(ErrorCodes::BadValue, "cannot compare to undefined");
    }

    switch (matchType()) {
        case LT:
        case LTE:
        case EQ:
        case GT:
        case GTE:
            break;
        default:
            return Status(ErrorCodes::BadValue, "bad match type for ComparisonMatchExpression");
    }

    return setPath(path);
}


bool ComparisonMatchExpression::matchesSingleElement(const BSONElement& e) const {
    // log() << "\t ComparisonMatchExpression e: " << e << " _rhs: " << _rhs << "\n"
    //<< toString() << std::endl;

    if (e.canonicalType() != _rhs.canonicalType()) {
        // some special cases
        //  jstNULL and undefined are treated the same
        if (e.canonicalType() + _rhs.canonicalType() == 5) {
            return matchType() == EQ || matchType() == LTE || matchType() == GTE;
        }

        if (_rhs.type() == MaxKey || _rhs.type() == MinKey) {
            return matchType() != EQ;
        }

        return false;
    }

    // Special case handling for NaN. NaN is equal to NaN but
    // otherwise always compares to false.
    if (std::isnan(e.numberDouble()) || std::isnan(_rhs.numberDouble())) {
        bool bothNaN = std::isnan(e.numberDouble()) && std::isnan(_rhs.numberDouble());
        switch (matchType()) {
            case LT:
                return false;
            case LTE:
                return bothNaN;
            case EQ:
                return bothNaN;
            case GT:
                return false;
            case GTE:
                return bothNaN;
            default:
                // This is a comparison match expression, so it must be either
                // a $lt, $lte, $gt, $gte, or equality expression.
                fassertFailed(17448);
        }
    }

    int x = compareElementValues(e, _rhs, _collator);

    // log() << "\t\t" << x << endl;

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

void ComparisonMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " ";
    switch (matchType()) {
        case LT:
            debug << "$lt";
            break;
        case LTE:
            debug << "$lte";
            break;
        case EQ:
            debug << "==";
            break;
        case GT:
            debug << "$gt";
            break;
        case GTE:
            debug << "$gte";
            break;
        default:
            invariant(false);
    }
    debug << " " << _rhs.toString(false);

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }

    debug << "\n";
}

void ComparisonMatchExpression::serialize(BSONObjBuilder* out) const {
    string opString = "";
    switch (matchType()) {
        case LT:
            opString = "$lt";
            break;
        case LTE:
            opString = "$lte";
            break;
        case EQ:
            opString = "$eq";
            break;
        case GT:
            opString = "$gt";
            break;
        case GTE:
            opString = "$gte";
            break;
        default:
            invariant(false);
    }

    out->append(path(), BSON(opString << _rhs));
}

// ---------------

// TODO: move
inline pcrecpp::RE_Options flags2options(const char* flags) {
    pcrecpp::RE_Options options;
    options.set_utf8(true);
    while (flags && *flags) {
        if (*flags == 'i')
            options.set_caseless(true);
        else if (*flags == 'm')
            options.set_multiline(true);
        else if (*flags == 'x')
            options.set_extended(true);
        else if (*flags == 's')
            options.set_dotall(true);
        flags++;
    }
    return options;
}

RegexMatchExpression::RegexMatchExpression() : LeafMatchExpression(REGEX) {}

RegexMatchExpression::~RegexMatchExpression() {}

bool RegexMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const RegexMatchExpression* realOther = static_cast<const RegexMatchExpression*>(other);
    return path() == realOther->path() && _regex == realOther->_regex &&
        _flags == realOther->_flags;
}


Status RegexMatchExpression::init(StringData path, const BSONElement& e) {
    if (e.type() != RegEx)
        return Status(ErrorCodes::BadValue, "regex not a regex");
    return init(path, e.regex(), e.regexFlags());
}


Status RegexMatchExpression::init(StringData path, StringData regex, StringData options) {
    if (regex.size() > MaxPatternSize) {
        return Status(ErrorCodes::BadValue, "Regular expression is too long");
    }

    if (regex.find('\0') != std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      "Regular expression cannot contain an embedded null byte");
    }

    if (options.find('\0') != std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      "Regular expression options string cannot contain an embedded null byte");
    }

    _regex = regex.toString();
    _flags = options.toString();
    _re.reset(new pcrecpp::RE(_regex.c_str(), flags2options(_flags.c_str())));

    return setPath(path);
}

bool RegexMatchExpression::matchesSingleElement(const BSONElement& e) const {
    switch (e.type()) {
        case String:
        case Symbol: {
            // String values stored in documents can contain embedded NUL bytes. We construct a
            // pcrecpp::StringPiece instance using the full length of the string to avoid truncating
            // 'data' early.
            pcrecpp::StringPiece data(e.valuestr(), e.valuestrsize() - 1);
            return _re->PartialMatch(data);
        }
        case RegEx:
            return _regex == e.regex() && _flags == e.regexFlags();
        default:
            return false;
    }
}

void RegexMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " regex /" << _regex << "/" << _flags;

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

void RegexMatchExpression::serialize(BSONObjBuilder* out) const {
    out->appendRegex(path(), _regex, _flags);
}

void RegexMatchExpression::shortDebugString(StringBuilder& debug) const {
    debug << "/" << _regex << "/" << _flags;
}

// ---------

Status ModMatchExpression::init(StringData path, int divisor, int remainder) {
    if (divisor == 0)
        return Status(ErrorCodes::BadValue, "divisor cannot be 0");
    _divisor = divisor;
    _remainder = remainder;
    return setPath(path);
}

bool ModMatchExpression::matchesSingleElement(const BSONElement& e) const {
    if (!e.isNumber())
        return false;
    return e.numberLong() % _divisor == _remainder;
}

void ModMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " mod " << _divisor << " % x == " << _remainder;
    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

void ModMatchExpression::serialize(BSONObjBuilder* out) const {
    out->append(path(), BSON("$mod" << BSON_ARRAY(_divisor << _remainder)));
}

bool ModMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ModMatchExpression* realOther = static_cast<const ModMatchExpression*>(other);
    return path() == realOther->path() && _divisor == realOther->_divisor &&
        _remainder == realOther->_remainder;
}


// ------------------

Status ExistsMatchExpression::init(StringData path) {
    return setPath(path);
}

bool ExistsMatchExpression::matchesSingleElement(const BSONElement& e) const {
    return !e.eoo();
}

void ExistsMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " exists";
    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

void ExistsMatchExpression::serialize(BSONObjBuilder* out) const {
    out->append(path(), BSON("$exists" << true));
}

bool ExistsMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ExistsMatchExpression* realOther = static_cast<const ExistsMatchExpression*>(other);
    return path() == realOther->path();
}


// ----

const std::string TypeMatchExpression::kMatchesAllNumbersAlias = "number";

const std::unordered_map<std::string, BSONType> TypeMatchExpression::typeAliasMap = {
    {typeName(NumberDouble), NumberDouble},
    {typeName(String), String},
    {typeName(Object), Object},
    {typeName(Array), Array},
    {typeName(BinData), BinData},
    {typeName(Undefined), Undefined},
    {typeName(jstOID), jstOID},
    {typeName(Bool), Bool},
    {typeName(Date), Date},
    {typeName(jstNULL), jstNULL},
    {typeName(RegEx), RegEx},
    {typeName(DBRef), DBRef},
    {typeName(Code), Code},
    {typeName(Symbol), Symbol},
    {typeName(CodeWScope), CodeWScope},
    {typeName(NumberInt), NumberInt},
    {typeName(bsonTimestamp), bsonTimestamp},
    {typeName(NumberLong), NumberLong},
    {typeName(NumberDecimal), NumberDecimal},
    {typeName(MaxKey), MaxKey},
    {typeName(MinKey), MinKey}};

Status TypeMatchExpression::initWithBSONType(StringData path, int type) {
    if (!isValidBSONType(type)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid numerical $type code: " << type);
    }

    _path = path;
    _type = static_cast<BSONType>(type);
    return _elementPath.init(_path);
}

Status TypeMatchExpression::initAsMatchingAllNumbers(StringData path) {
    _path = path;
    _matchesAllNumbers = true;
    return _elementPath.init(_path);
}

bool TypeMatchExpression::matchesSingleElement(const BSONElement& e) const {
    if (_matchesAllNumbers) {
        return e.isNumber();
    }

    return e.type() == _type;
}

bool TypeMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    MatchableDocument::IteratorHolder cursor(doc, &_elementPath);
    while (cursor->more()) {
        ElementIterator::Context e = cursor->next();

        // In the case where _elementPath is referring to an array,
        // $type should match elements of that array only.
        // outerArray() helps to identify elements of the array
        // and the containing array itself.
        // This matters when we are looking for {$type: Array}.
        // Example (_elementPath refers to field 'a' and _type is Array):
        // a : [        // outer array. should not match
        //     123,     // inner array
        //     [ 456 ], // inner array. should match
        //     ...
        // ]
        if (_type == mongo::Array && e.outerArray()) {
            continue;
        }

        if (!matchesSingleElement(e.element())) {
            continue;
        }

        if (details && details->needRecord() && !e.arrayOffset().eoo()) {
            details->setElemMatchKey(e.arrayOffset().fieldName());
        }
        return true;
    }
    return false;
}

void TypeMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << _path << " type: ";
    if (matchesAllNumbers()) {
        debug << kMatchesAllNumbersAlias;
    } else {
        debug << _type;
    }

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

void TypeMatchExpression::serialize(BSONObjBuilder* out) const {
    if (matchesAllNumbers()) {
        out->append(path(), BSON("$type" << kMatchesAllNumbersAlias));
    } else {
        out->append(path(), BSON("$type" << _type));
    }
}

bool TypeMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const TypeMatchExpression* realOther = static_cast<const TypeMatchExpression*>(other);

    if (_path != realOther->_path) {
        return false;
    }

    if (_matchesAllNumbers) {
        return realOther->_matchesAllNumbers;
    }

    return _type == realOther->_type;
}


// -----------

Status InMatchExpression::init(StringData path) {
    return setPath(path);
}

std::unique_ptr<MatchExpression> InMatchExpression::shallowClone() const {
    auto next = stdx::make_unique<InMatchExpression>();
    next->init(path());
    next->setCollator(_collator);
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    next->_hasNull = _hasNull;
    next->_hasEmptyArray = _hasEmptyArray;
    next->_equalitySet = _equalitySet;
    next->_originalEqualityVector = _originalEqualityVector;
    for (auto&& regex : _regexes) {
        std::unique_ptr<RegexMatchExpression> clonedRegex(
            static_cast<RegexMatchExpression*>(regex->shallowClone().release()));
        next->_regexes.push_back(std::move(clonedRegex));
    }
    return std::move(next);
}

bool InMatchExpression::matchesSingleElement(const BSONElement& e) const {
    if (_hasNull && e.eoo()) {
        return true;
    }
    if (_equalitySet.find(e) != _equalitySet.end()) {
        return true;
    }
    for (auto&& regex : _regexes) {
        if (regex->matchesSingleElement(e)) {
            return true;
        }
    }
    return false;
}

void InMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
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
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

void InMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder inBob(out->subobjStart(path()));
    BSONArrayBuilder arrBob(inBob.subarrayStart("$in"));
    for (auto&& _equality : _equalitySet) {
        arrBob.append(_equality);
    }
    for (auto&& _regex : _regexes) {
        BSONObjBuilder regexBob;
        _regex->serialize(&regexBob);
        arrBob.append(regexBob.obj().firstElement());
    }
    arrBob.doneFast();
    inBob.doneFast();
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

    // We need to re-compute '_equalitySet', since our set comparator has changed.
    BSONElementSet equalitiesWithNewComparator(
        _originalEqualityVector.begin(), _originalEqualityVector.end(), collator);
    _equalitySet = std::move(equalitiesWithNewComparator);
}

Status InMatchExpression::addEquality(const BSONElement& elt) {
    if (elt.type() == BSONType::RegEx) {
        return Status(ErrorCodes::BadValue, "InMatchExpression equality cannot be a regex");
    }
    if (elt.type() == BSONType::Undefined) {
        return Status(ErrorCodes::BadValue, "InMatchExpression equality cannot be undefined");
    }

    if (elt.type() == BSONType::jstNULL) {
        _hasNull = true;
    }
    if (elt.type() == BSONType::Array && elt.Obj().isEmpty()) {
        _hasEmptyArray = true;
    }
    _equalitySet.insert(elt);
    _originalEqualityVector.push_back(elt);
    return Status::OK();
}

Status InMatchExpression::addRegex(std::unique_ptr<RegexMatchExpression> expr) {
    _regexes.push_back(std::move(expr));
    return Status::OK();
}

// -----------

const double BitTestMatchExpression::kLongLongMaxPlusOneAsDouble =
    scalbn(1, std::numeric_limits<long long>::digits);

Status BitTestMatchExpression::init(StringData path, std::vector<uint32_t> bitPositions) {
    _bitPositions = std::move(bitPositions);

    // Process bit positions into bitmask.
    for (auto bitPosition : _bitPositions) {
        // Checking bits > 63 is just checking the sign bit, since we sign-extend numbers. For
        // example, the 100th bit of -1 is considered set if and only if the 63rd bit position is
        // set.
        bitPosition = std::min(bitPosition, 63U);
        _bitMask |= 1ULL << bitPosition;
    }

    return setPath(path);
}

Status BitTestMatchExpression::init(StringData path, uint64_t bitMask) {
    _bitMask = bitMask;

    // Process bitmask into bit positions.
    for (int bit = 0; bit < 64; bit++) {
        if (_bitMask & (1ULL << bit)) {
            _bitPositions.push_back(bit);
        }
    }

    return setPath(path);
}

Status BitTestMatchExpression::init(StringData path,
                                    const char* bitMaskBinary,
                                    uint32_t bitMaskLen) {
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

    return setPath(path);
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
            invariant(false);
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

bool BitTestMatchExpression::matchesSingleElement(const BSONElement& e) const {
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
        if (eDouble >= kLongLongMaxPlusOneAsDouble ||
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

void BitTestMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);

    debug << path() << " ";

    switch (matchType()) {
        case BITS_ALL_SET:
            debug << "$bitsAllSet:";
            break;
        case BITS_ALL_CLEAR:
            debug << "$bitsAllClear:";
            break;
        case BITS_ANY_SET:
            debug << "$bitsAnySet:";
            break;
        case BITS_ANY_CLEAR:
            debug << "$bitsAnyClear:";
            break;
        default:
            invariant(false);
    }

    debug << " [";
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

void BitTestMatchExpression::serialize(BSONObjBuilder* out) const {
    string opString = "";

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
            invariant(false);
    }

    BSONArrayBuilder arrBob;
    for (auto bitPosition : _bitPositions) {
        arrBob.append(bitPosition);
    }
    arrBob.doneFast();

    out->append(path(), BSON(opString << arrBob.arr()));
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
}
