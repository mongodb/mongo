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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/query/index_bounds_builder.h"

#include <cmath>
#include <limits>

#include "mongo/base/string_data.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/s2.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/expression_index.h"
#include "mongo/db/query/expression_index_knobs.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2regioncoverer.h"

namespace mongo {

namespace {

// Tightness rules are shared for $lt, $lte, $gt, $gte.
IndexBoundsBuilder::BoundsTightness getInequalityPredicateTightness(const BSONElement& dataElt,
                                                                    const IndexEntry& index) {
    // TODO SERVER-23093: Right now we consider a string comparison in the presence of a
    // collator to be inexact fetch, since such queries cannot be covered. Although it is
    // necessary to fetch the keyed documents, it is not necessary to reapply the filter.
    if (CollationIndexKey::shouldUseCollationIndexKey(dataElt, index.collator)) {
        return IndexBoundsBuilder::INEXACT_FETCH;
    }

    if (dataElt.isSimpleType() || dataElt.type() == BSONType::BinData) {
        return IndexBoundsBuilder::EXACT;
    }

    return IndexBoundsBuilder::INEXACT_FETCH;
}

}  // namespace

string IndexBoundsBuilder::simpleRegex(const char* regex,
                                       const char* flags,
                                       const IndexEntry& index,
                                       BoundsTightness* tightnessOut) {
    if (index.collator) {
        // Bounds building for simple regular expressions assumes that the index is in ASCII order,
        // which is not necessarily true for an index with a collator.  Therefore, a regex can never
        // use tight bounds if the index has a non-null collator. In this case, the regex must be
        // applied to the fetched document rather than the index key, so the tightness is
        // INEXACT_FETCH.
        *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        return "";
    }

    string r = "";
    *tightnessOut = IndexBoundsBuilder::INEXACT_COVERED;

    bool multilineOK;
    if (regex[0] == '\\' && regex[1] == 'A') {
        multilineOK = true;
        regex += 2;
    } else if (regex[0] == '^') {
        multilineOK = false;
        regex += 1;
    } else {
        return r;
    }

    // A regex with the "|" character is never considered a simple regular expression.
    if (StringData(regex).find('|') != std::string::npos) {
        return "";
    }

    bool extended = false;
    while (*flags) {
        switch (*(flags++)) {
            case 'm':  // multiline
                if (multilineOK)
                    continue;
                else
                    return r;
            case 's':
                // Single-line mode specified. This just changes the behavior of the '.'
                // character to match every character instead of every character except '\n'.
                continue;
            case 'x':  // extended
                extended = true;
                break;
            default:
                return r;  // cant use index
        }
    }

    mongoutils::str::stream ss;

    while (*regex) {
        char c = *(regex++);

        // We should have bailed out early above if '|' is in the regex.
        invariant(c != '|');

        if (c == '*' || c == '?') {
            // These are the only two symbols that make the last char optional
            r = ss;
            r = r.substr(0, r.size() - 1);
            return r;  // breaking here fails with /^a?/
        } else if (c == '\\') {
            c = *(regex++);
            if (c == 'Q') {
                // \Q...\E quotes everything inside
                while (*regex) {
                    c = (*regex++);
                    if (c == '\\' && (*regex == 'E')) {
                        regex++;  // skip the 'E'
                        break;    // go back to start of outer loop
                    } else {
                        ss << c;  // character should match itself
                    }
                }
            } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '0') ||
                       (c == '\0')) {
                // don't know what to do with these
                r = ss;
                break;
            } else {
                // slash followed by non-alphanumeric represents the following char
                ss << c;
            }
        } else if (strchr("^$.[()+{", c)) {
            // list of "metacharacters" from man pcrepattern
            r = ss;
            break;
        } else if (extended && c == '#') {
            // comment
            r = ss;
            break;
        } else if (extended && isspace(c)) {
            continue;
        } else {
            // self-matching char
            ss << c;
        }
    }

    if (r.empty() && *regex == 0) {
        r = ss;
        *tightnessOut = r.empty() ? IndexBoundsBuilder::INEXACT_COVERED : IndexBoundsBuilder::EXACT;
    }

    return r;
}


// static
void IndexBoundsBuilder::allValuesForField(const BSONElement& elt, OrderedIntervalList* out) {
    // ARGH, BSONValue would make this shorter.
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");
    out->name = elt.fieldName();
    out->intervals.push_back(makeRangeInterval(bob.obj(), true, true));
}

Interval IndexBoundsBuilder::allValues() {
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");
    return makeRangeInterval(bob.obj(), true, true);
}

bool IntervalComparison(const Interval& lhs, const Interval& rhs) {
    int wo = lhs.start.woCompare(rhs.start, false);
    if (0 != wo) {
        return wo < 0;
    }

    // The start and end are equal.
    // Strict weak requires irreflexivity which implies that equivalence returns false.
    if (lhs.startInclusive == rhs.startInclusive) {
        return false;
    }

    // Put the bound that's inclusive to the left.
    return lhs.startInclusive;
}

// static
void IndexBoundsBuilder::translateAndIntersect(const MatchExpression* expr,
                                               const BSONElement& elt,
                                               const IndexEntry& index,
                                               OrderedIntervalList* oilOut,
                                               BoundsTightness* tightnessOut) {
    OrderedIntervalList arg;
    translate(expr, elt, index, &arg, tightnessOut);

    // translate outputs arg in sorted order.  intersectize assumes that its arguments are
    // sorted.
    intersectize(arg, oilOut);
}

// static
void IndexBoundsBuilder::translateAndUnion(const MatchExpression* expr,
                                           const BSONElement& elt,
                                           const IndexEntry& index,
                                           OrderedIntervalList* oilOut,
                                           BoundsTightness* tightnessOut) {
    OrderedIntervalList arg;
    translate(expr, elt, index, &arg, tightnessOut);

    // Append the new intervals to oilOut.
    oilOut->intervals.insert(oilOut->intervals.end(), arg.intervals.begin(), arg.intervals.end());

    // Union the appended intervals with the existing ones.
    unionize(oilOut);
}

bool typeMatch(const BSONObj& obj) {
    BSONObjIterator it(obj);
    verify(it.more());
    BSONElement first = it.next();
    verify(it.more());
    BSONElement second = it.next();
    return first.canonicalType() == second.canonicalType();
}

// static
void IndexBoundsBuilder::translate(const MatchExpression* expr,
                                   const BSONElement& elt,
                                   const IndexEntry& index,
                                   OrderedIntervalList* oilOut,
                                   BoundsTightness* tightnessOut) {
    // We expect that the OIL we are constructing starts out empty.
    invariant(oilOut->intervals.empty());

    oilOut->name = elt.fieldName();

    bool isHashed = false;
    if (mongoutils::str::equals("hashed", elt.valuestrsafe())) {
        isHashed = true;
    }

    if (isHashed) {
        verify(MatchExpression::EQ == expr->matchType() ||
               MatchExpression::MATCH_IN == expr->matchType());
    }

    if (MatchExpression::ELEM_MATCH_VALUE == expr->matchType()) {
        OrderedIntervalList acc;
        translate(expr->getChild(0), elt, index, &acc, tightnessOut);

        for (size_t i = 1; i < expr->numChildren(); ++i) {
            OrderedIntervalList next;
            BoundsTightness tightness;
            translate(expr->getChild(i), elt, index, &next, &tightness);
            intersectize(next, &acc);
        }

        for (size_t i = 0; i < acc.intervals.size(); ++i) {
            oilOut->intervals.push_back(acc.intervals[i]);
        }

        if (!oilOut->intervals.empty()) {
            std::sort(oilOut->intervals.begin(), oilOut->intervals.end(), IntervalComparison);
        }

        // $elemMatch value requires an array.
        // Scalars and directly nested objects are not matched with $elemMatch.
        // We can't tell if a multi-key index key is derived from an array field.
        // Therefore, a fetch is required.
        *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
    } else if (MatchExpression::NOT == expr->matchType()) {
        // A NOT is indexed by virtue of its child. If we're here then the NOT's child
        // must be a kind of node for which we can index negations. It can't be things like
        // $mod, $regex, or $type.
        MatchExpression* child = expr->getChild(0);

        // If we have a NOT -> EXISTS, we must handle separately.
        if (MatchExpression::EXISTS == child->matchType()) {
            // We should never try to use a sparse index for $exists:false.
            invariant(!index.sparse);
            BSONObjBuilder bob;
            bob.appendNull("");
            bob.appendNull("");
            BSONObj dataObj = bob.obj();
            oilOut->intervals.push_back(makeRangeInterval(dataObj, true, true));

            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
            return;
        }

        translate(child, elt, index, oilOut, tightnessOut);
        oilOut->complement();

        // If the index is multikey, it doesn't matter what the tightness of the child is, we must
        // return INEXACT_FETCH. Consider a multikey index on 'a' with document {a: [1, 2, 3]} and
        // query {a: {$ne: 3}}.  If we treated the bounds [MinKey, 3), (3, MaxKey] as exact, then we
        // would erroneously return the document!
        //
        // If the index has a collator, then complementing the bounds generally results in strings
        // being in-bounds. Such index bounds cannot be used in a covered plan, since we should
        // never return collator comparison keys to the user. As such, we must make the bounds
        // INEXACT_FETCH in this case.
        //
        // TODO SERVER-23093: Although it is necessary to fetch the keyed documents, it is not
        // necessary to reapply the filter.
        if (index.multikey || index.collator) {
            *tightnessOut = INEXACT_FETCH;
        }
    } else if (MatchExpression::EXISTS == expr->matchType()) {
        oilOut->intervals.push_back(allValues());

        // We only handle the {$exists:true} case, as {$exists:false}
        // will have been translated to {$not:{ $exists:true }}.
        //
        // Documents with a missing value are stored *as if* they were
        // explicitly given the value 'null'.  Given:
        //    X = { b : 1 }
        //    Y = { a : null, b : 1 }
        // X and Y look identical from within a standard index on { a : 1 }.
        // HOWEVER a sparse index on { a : 1 } will treat X and Y differently,
        // storing Y and not storing X.
        //
        // We can safely use an index in the following cases:
        // {a:{ $exists:true }} - normal index helps, but we must still fetch
        // {a:{ $exists:true }} - sparse index is exact
        // {a:{ $exists:false }} - normal index requires a fetch
        // {a:{ $exists:false }} - sparse indexes cannot be used at all.
        //
        // Noted in SERVER-12869, in case this ever changes some day.
        //
        // Bounds are always INEXACT_FETCH if there is a collator on the index, since the bounds
        // include collator-generated sort keys that shouldn't be returned to the user.
        //
        // TODO SERVER-23093: Although it is necessary to fetch the keyed documents when there is a
        // collator, it is not necessary to reapply the filter.
        if (index.sparse && !index.collator) {
            // A sparse, compound index on { a:1, b:1 } will include entries
            // for all of the following documents:
            //    { a:1 }, { b:1 }, { a:1, b:1 }
            // So we must use INEXACT bounds in this case.
            if (1 < index.keyPattern.nFields()) {
                *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
            } else {
                *tightnessOut = IndexBoundsBuilder::EXACT;
            }
        } else {
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        }
    } else if (MatchExpression::EQ == expr->matchType()) {
        const EqualityMatchExpression* node = static_cast<const EqualityMatchExpression*>(expr);
        translateEquality(node->getData(), index, isHashed, oilOut, tightnessOut);
    } else if (MatchExpression::LTE == expr->matchType()) {
        const LTEMatchExpression* node = static_cast<const LTEMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Everything is <= MaxKey.
        if (MaxKey == dataElt.type()) {
            oilOut->intervals.push_back(allValues());
            *tightnessOut =
                index.collator ? IndexBoundsBuilder::INEXACT_FETCH : IndexBoundsBuilder::EXACT;
            return;
        }

        // Only NaN is <= NaN.
        if (std::isnan(dataElt.numberDouble())) {
            double nan = dataElt.numberDouble();
            oilOut->intervals.push_back(makePointInterval(nan));
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        BSONObjBuilder bob;
        // Use -infinity for one-sided numerical bounds
        if (dataElt.isNumber()) {
            bob.appendNumber("", -std::numeric_limits<double>::infinity());
        } else {
            bob.appendMinForType("", dataElt.type());
        }
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, index.collator, &bob);
        BSONObj dataObj = bob.obj();
        verify(dataObj.isOwned());
        oilOut->intervals.push_back(makeRangeInterval(dataObj, typeMatch(dataObj), true));

        *tightnessOut = getInequalityPredicateTightness(dataElt, index);
    } else if (MatchExpression::LT == expr->matchType()) {
        const LTMatchExpression* node = static_cast<const LTMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Everything is <= MaxKey.
        if (MaxKey == dataElt.type()) {
            oilOut->intervals.push_back(allValues());
            *tightnessOut =
                index.collator ? IndexBoundsBuilder::INEXACT_FETCH : IndexBoundsBuilder::EXACT;
            return;
        }

        // Nothing is < NaN.
        if (std::isnan(dataElt.numberDouble())) {
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        BSONObjBuilder bob;
        // Use -infinity for one-sided numerical bounds
        if (dataElt.isNumber()) {
            bob.appendNumber("", -std::numeric_limits<double>::infinity());
        } else {
            bob.appendMinForType("", dataElt.type());
        }
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, index.collator, &bob);
        BSONObj dataObj = bob.obj();
        verify(dataObj.isOwned());
        Interval interval = makeRangeInterval(dataObj, typeMatch(dataObj), false);

        // If the operand to LT is equal to the lower bound X, the interval [X, X) is invalid
        // and should not be added to the bounds.
        if (!interval.isNull()) {
            oilOut->intervals.push_back(interval);
        }

        *tightnessOut = getInequalityPredicateTightness(dataElt, index);
    } else if (MatchExpression::GT == expr->matchType()) {
        const GTMatchExpression* node = static_cast<const GTMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Everything is > MinKey.
        if (MinKey == dataElt.type()) {
            oilOut->intervals.push_back(allValues());
            *tightnessOut =
                index.collator ? IndexBoundsBuilder::INEXACT_FETCH : IndexBoundsBuilder::EXACT;
            return;
        }

        // Nothing is > NaN.
        if (std::isnan(dataElt.numberDouble())) {
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        BSONObjBuilder bob;
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, index.collator, &bob);
        if (dataElt.isNumber()) {
            bob.appendNumber("", std::numeric_limits<double>::infinity());
        } else {
            bob.appendMaxForType("", dataElt.type());
        }
        BSONObj dataObj = bob.obj();
        verify(dataObj.isOwned());
        Interval interval = makeRangeInterval(dataObj, false, typeMatch(dataObj));

        // If the operand to GT is equal to the upper bound X, the interval (X, X] is invalid
        // and should not be added to the bounds.
        if (!interval.isNull()) {
            oilOut->intervals.push_back(interval);
        }

        *tightnessOut = getInequalityPredicateTightness(dataElt, index);
    } else if (MatchExpression::GTE == expr->matchType()) {
        const GTEMatchExpression* node = static_cast<const GTEMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Everything is >= MinKey.
        if (MinKey == dataElt.type()) {
            oilOut->intervals.push_back(allValues());
            *tightnessOut =
                index.collator ? IndexBoundsBuilder::INEXACT_FETCH : IndexBoundsBuilder::EXACT;
            return;
        }

        // Only NaN is >= NaN.
        if (std::isnan(dataElt.numberDouble())) {
            double nan = dataElt.numberDouble();
            oilOut->intervals.push_back(makePointInterval(nan));
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        BSONObjBuilder bob;
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, index.collator, &bob);
        if (dataElt.isNumber()) {
            bob.appendNumber("", std::numeric_limits<double>::infinity());
        } else {
            bob.appendMaxForType("", dataElt.type());
        }
        BSONObj dataObj = bob.obj();
        verify(dataObj.isOwned());

        oilOut->intervals.push_back(makeRangeInterval(dataObj, true, typeMatch(dataObj)));

        *tightnessOut = getInequalityPredicateTightness(dataElt, index);
    } else if (MatchExpression::REGEX == expr->matchType()) {
        const RegexMatchExpression* rme = static_cast<const RegexMatchExpression*>(expr);
        translateRegex(rme, index, oilOut, tightnessOut);
    } else if (MatchExpression::MOD == expr->matchType()) {
        BSONObjBuilder bob;
        bob.appendMinForType("", NumberDouble);
        bob.appendMaxForType("", NumberDouble);
        BSONObj dataObj = bob.obj();
        verify(dataObj.isOwned());
        oilOut->intervals.push_back(makeRangeInterval(dataObj, true, true));
        *tightnessOut = IndexBoundsBuilder::INEXACT_COVERED;
    } else if (MatchExpression::TYPE_OPERATOR == expr->matchType()) {
        const TypeMatchExpression* tme = static_cast<const TypeMatchExpression*>(expr);

        // If we are matching all numbers, we just use the bounds for NumberInt, as these bounds
        // also include all NumberDouble and NumberLong values.
        BSONType type = tme->matchesAllNumbers() ? BSONType::NumberInt : tme->getType();
        BSONObjBuilder bob;
        bob.appendMinForType("", type);
        bob.appendMaxForType("", type);
        BSONObj dataObj = bob.obj();
        verify(dataObj.isOwned());
        oilOut->intervals.push_back(makeRangeInterval(dataObj, true, true));

        *tightnessOut = tme->matchesAllNumbers() ? IndexBoundsBuilder::EXACT
                                                 : IndexBoundsBuilder::INEXACT_FETCH;
    } else if (MatchExpression::MATCH_IN == expr->matchType()) {
        const InMatchExpression* ime = static_cast<const InMatchExpression*>(expr);

        *tightnessOut = IndexBoundsBuilder::EXACT;

        // Create our various intervals.

        IndexBoundsBuilder::BoundsTightness tightness;
        for (auto&& equality : ime->getEqualities()) {
            translateEquality(equality, index, isHashed, oilOut, &tightness);
            if (tightness != IndexBoundsBuilder::EXACT) {
                *tightnessOut = tightness;
            }
        }

        for (auto&& regex : ime->getRegexes()) {
            translateRegex(regex.get(), index, oilOut, &tightness);
            if (tightness != IndexBoundsBuilder::EXACT) {
                *tightnessOut = tightness;
            }
        }

        if (ime->hasNull()) {
            // A null index key does not always match a null query value so we must fetch the
            // doc and run a full comparison.  See SERVER-4529.
            // TODO: Do we already set the tightnessOut by calling translateEquality?
            *tightnessOut = INEXACT_FETCH;
        }

        if (ime->hasEmptyArray()) {
            // Empty arrays are indexed as undefined.
            BSONObjBuilder undefinedBob;
            undefinedBob.appendUndefined("");
            oilOut->intervals.push_back(makePointInterval(undefinedBob.obj()));
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        }

        unionize(oilOut);
    } else if (MatchExpression::GEO == expr->matchType()) {
        const GeoMatchExpression* gme = static_cast<const GeoMatchExpression*>(expr);

        if (mongoutils::str::equals("2dsphere", elt.valuestrsafe())) {
            verify(gme->getGeoExpression().getGeometry().hasS2Region());
            const S2Region& region = gme->getGeoExpression().getGeometry().getS2Region();
            S2IndexingParams indexParams;
            ExpressionParams::initialize2dsphereParams(index.infoObj, index.collator, &indexParams);
            ExpressionMapping::cover2dsphere(region, indexParams, oilOut);
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        } else if (mongoutils::str::equals("2d", elt.valuestrsafe())) {
            verify(gme->getGeoExpression().getGeometry().hasR2Region());
            const R2Region& region = gme->getGeoExpression().getGeometry().getR2Region();

            ExpressionMapping::cover2d(
                region, index.infoObj, internalGeoPredicateQuery2DMaxCoveringCells, oilOut);

            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        } else {
            warning() << "Planner error trying to build geo bounds for " << elt.toString()
                      << " index element.";
            verify(0);
        }
    } else {
        warning() << "Planner error, trying to build bounds for expression: " << expr->toString()
                  << endl;
        verify(0);
    }
}

// static
Interval IndexBoundsBuilder::makeRangeInterval(const BSONObj& obj,
                                               bool startInclusive,
                                               bool endInclusive) {
    Interval ret;
    ret._intervalData = obj;
    ret.startInclusive = startInclusive;
    ret.endInclusive = endInclusive;
    BSONObjIterator it(obj);
    verify(it.more());
    ret.start = it.next();
    verify(it.more());
    ret.end = it.next();
    return ret;
}

// static
void IndexBoundsBuilder::intersectize(const OrderedIntervalList& arg, OrderedIntervalList* oilOut) {
    verify(arg.name == oilOut->name);

    size_t argidx = 0;
    const vector<Interval>& argiv = arg.intervals;

    size_t ividx = 0;
    vector<Interval>& iv = oilOut->intervals;

    vector<Interval> result;

    while (argidx < argiv.size() && ividx < iv.size()) {
        Interval::IntervalComparison cmp = argiv[argidx].compare(iv[ividx]);

        verify(Interval::INTERVAL_UNKNOWN != cmp);

        if (cmp == Interval::INTERVAL_PRECEDES || cmp == Interval::INTERVAL_PRECEDES_COULD_UNION) {
            // argiv is before iv.  move argiv forward.
            ++argidx;
        } else if (cmp == Interval::INTERVAL_SUCCEEDS) {
            // iv is before argiv.  move iv forward.
            ++ividx;
        } else {
            // argiv[argidx] (cmpresults) iv[ividx]
            Interval newInt = argiv[argidx];
            newInt.intersect(iv[ividx], cmp);
            result.push_back(newInt);

            if (Interval::INTERVAL_EQUALS == cmp) {
                ++argidx;
                ++ividx;
            } else if (Interval::INTERVAL_WITHIN == cmp) {
                ++argidx;
            } else if (Interval::INTERVAL_CONTAINS == cmp) {
                ++ividx;
            } else if (Interval::INTERVAL_OVERLAPS_BEFORE == cmp) {
                ++argidx;
            } else if (Interval::INTERVAL_OVERLAPS_AFTER == cmp) {
                ++ividx;
            } else {
                verify(0);
            }
        }
    }

    oilOut->intervals.swap(result);
}

// static
void IndexBoundsBuilder::unionize(OrderedIntervalList* oilOut) {
    vector<Interval>& iv = oilOut->intervals;

    // This can happen.
    if (iv.empty()) {
        return;
    }

    // Step 1: sort.
    std::sort(iv.begin(), iv.end(), IntervalComparison);

    // Step 2: Walk through and merge.
    size_t i = 0;
    while (i < iv.size() - 1) {
        // Compare i with i + 1.
        Interval::IntervalComparison cmp = iv[i].compare(iv[i + 1]);

        // This means our sort didn't work.
        verify(Interval::INTERVAL_SUCCEEDS != cmp);

        // Intervals are correctly ordered.
        if (Interval::INTERVAL_PRECEDES == cmp) {
            // We can move to the next pair.
            ++i;
        } else if (Interval::INTERVAL_EQUALS == cmp || Interval::INTERVAL_WITHIN == cmp) {
            // Interval 'i' is equal to i+1, or is contained within i+1.
            // Remove interval i and don't move to the next value of 'i'.
            iv.erase(iv.begin() + i);
        } else if (Interval::INTERVAL_CONTAINS == cmp) {
            // Interval 'i' contains i+1, remove i+1 and don't move to the next value of 'i'.
            iv.erase(iv.begin() + i + 1);
        } else if (Interval::INTERVAL_OVERLAPS_BEFORE == cmp ||
                   Interval::INTERVAL_PRECEDES_COULD_UNION == cmp) {
            // We want to merge intervals i and i+1.
            // Interval 'i' starts before interval 'i+1'.
            BSONObjBuilder bob;
            bob.appendAs(iv[i].start, "");
            bob.appendAs(iv[i + 1].end, "");
            BSONObj data = bob.obj();
            bool startInclusive = iv[i].startInclusive;
            bool endInclusive = iv[i + 1].endInclusive;
            iv.erase(iv.begin() + i);
            // iv[i] is now the former iv[i + 1]
            iv[i] = makeRangeInterval(data, startInclusive, endInclusive);
            // Don't increment 'i'.
        }
    }
}

// static
Interval IndexBoundsBuilder::makeRangeInterval(const string& start,
                                               const string& end,
                                               bool startInclusive,
                                               bool endInclusive) {
    BSONObjBuilder bob;
    bob.append("", start);
    bob.append("", end);
    return makeRangeInterval(bob.obj(), startInclusive, endInclusive);
}

// static
Interval IndexBoundsBuilder::makePointInterval(const BSONObj& obj) {
    Interval ret;
    ret._intervalData = obj;
    ret.startInclusive = ret.endInclusive = true;
    ret.start = ret.end = obj.firstElement();
    return ret;
}

// static
Interval IndexBoundsBuilder::makePointInterval(const string& str) {
    BSONObjBuilder bob;
    bob.append("", str);
    return makePointInterval(bob.obj());
}

// static
Interval IndexBoundsBuilder::makePointInterval(double d) {
    BSONObjBuilder bob;
    bob.append("", d);
    return makePointInterval(bob.obj());
}

// static
BSONObj IndexBoundsBuilder::objFromElement(const BSONElement& elt,
                                           const CollatorInterface* collator) {
    BSONObjBuilder bob;
    CollationIndexKey::collationAwareIndexKeyAppend(elt, collator, &bob);
    return bob.obj();
}

// static
void IndexBoundsBuilder::reverseInterval(Interval* ival) {
    BSONElement tmp = ival->start;
    ival->start = ival->end;
    ival->end = tmp;

    bool tmpInc = ival->startInclusive;
    ival->startInclusive = ival->endInclusive;
    ival->endInclusive = tmpInc;
}

// static
void IndexBoundsBuilder::translateRegex(const RegexMatchExpression* rme,
                                        const IndexEntry& index,
                                        OrderedIntervalList* oilOut,
                                        BoundsTightness* tightnessOut) {
    const string start =
        simpleRegex(rme->getString().c_str(), rme->getFlags().c_str(), index, tightnessOut);

    // Note that 'tightnessOut' is set by simpleRegex above.
    if (!start.empty()) {
        string end = start;
        end[end.size() - 1]++;
        oilOut->intervals.push_back(makeRangeInterval(start, end, true, false));
    } else {
        BSONObjBuilder bob;
        bob.appendMinForType("", String);
        bob.appendMaxForType("", String);
        BSONObj dataObj = bob.obj();
        verify(dataObj.isOwned());
        oilOut->intervals.push_back(makeRangeInterval(dataObj, true, false));
    }

    // Regexes are after strings.
    BSONObjBuilder bob;
    bob.appendRegex("", rme->getString(), rme->getFlags());
    oilOut->intervals.push_back(makePointInterval(bob.obj()));
}

// static
void IndexBoundsBuilder::translateEquality(const BSONElement& data,
                                           const IndexEntry& index,
                                           bool isHashed,
                                           OrderedIntervalList* oil,
                                           BoundsTightness* tightnessOut) {
    // We have to copy the data out of the parse tree and stuff it into the index
    // bounds.  BSONValue will be useful here.
    if (Array != data.type()) {
        BSONObj dataObj = objFromElement(data, index.collator);
        if (isHashed) {
            dataObj = ExpressionMapping::hash(dataObj.firstElement());
        }

        verify(dataObj.isOwned());
        oil->intervals.push_back(makePointInterval(dataObj));

        // TODO SERVER-23093: Right now we consider a string comparison in the presence of a
        // collator to be inexact fetch, since such queries cannot be covered. Although it is
        // necessary to fetch the keyed documents, it is not necessary to reapply the filter.
        if (CollationIndexKey::shouldUseCollationIndexKey(data, index.collator)) {
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
            return;
        }

        if (dataObj.firstElement().isNull() || isHashed) {
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        } else {
            *tightnessOut = IndexBoundsBuilder::EXACT;
        }
        return;
    }

    // If we're here, Array == data.type().
    //
    // Using arrays with hashed indices is currently not supported, so we don't have to worry
    // about that case.
    //
    // Arrays are indexed by either:
    //
    // 1. the first element if there is one.  Note that using the first is arbitrary; we could
    // just as well use any array element.). If the query is {a: [1, 2, 3]}, for example, then
    // using the bounds [1, 1] for the multikey index will pick up every document containing the
    // array [1, 2, 3].
    //
    // 2. undefined if the array is empty.
    //
    // Also, arrays are indexed by:
    //
    // 3. the full array if it's inside of another array.  We check for this so that the query
    // {a: [1, 2, 3]} will match documents like {a: [[1, 2, 3], 4, 5]}.

    // Case 3.
    oil->intervals.push_back(makePointInterval(objFromElement(data, index.collator)));

    if (data.Obj().isEmpty()) {
        // Case 2.
        BSONObjBuilder undefinedBob;
        undefinedBob.appendUndefined("");
        oil->intervals.push_back(makePointInterval(undefinedBob.obj()));
    } else {
        // Case 1.
        BSONElement firstEl = data.Obj().firstElement();
        oil->intervals.push_back(makePointInterval(objFromElement(firstEl, index.collator)));
    }

    std::sort(oil->intervals.begin(), oil->intervals.end(), IntervalComparison);
    *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
}

// static
void IndexBoundsBuilder::allValuesBounds(const BSONObj& keyPattern, IndexBounds* bounds) {
    bounds->fields.resize(keyPattern.nFields());

    BSONObjIterator it(keyPattern);
    int field = 0;
    while (it.more()) {
        IndexBoundsBuilder::allValuesForField(it.next(), &bounds->fields[field]);
        ++field;
    }

    alignBounds(bounds, keyPattern);
}

// static
void IndexBoundsBuilder::alignBounds(IndexBounds* bounds, const BSONObj& kp, int scanDir) {
    BSONObjIterator it(kp);
    size_t oilIdx = 0;
    while (it.more()) {
        BSONElement elt = it.next();
        // The canonical check as to whether a key pattern element is "ascending" or "descending" is
        // (elt.number() >= 0). This is defined by the Ordering class.
        int direction = (elt.number() >= 0) ? 1 : -1;
        direction *= scanDir;
        if (-1 == direction) {
            vector<Interval>& iv = bounds->fields[oilIdx].intervals;
            // Step 1: reverse the list.
            std::reverse(iv.begin(), iv.end());
            // Step 2: reverse each interval.
            for (size_t i = 0; i < iv.size(); ++i) {
                iv[i].reverse();
            }
        }
        ++oilIdx;
    }

    if (!bounds->isValidFor(kp, scanDir)) {
        log() << "INVALID BOUNDS: " << bounds->toString() << endl
              << "kp = " << kp.toString() << endl
              << "scanDir = " << scanDir << endl;
        invariant(0);
    }
}

// static
bool IndexBoundsBuilder::isSingleInterval(const IndexBounds& bounds,
                                          BSONObj* startKey,
                                          bool* startKeyInclusive,
                                          BSONObj* endKey,
                                          bool* endKeyInclusive) {
    // We build our start/end keys as we go.
    BSONObjBuilder startBob;
    BSONObjBuilder endBob;

    // The start and end keys are inclusive unless we have a non-point interval, in which case
    // we take the inclusivity from there.
    *startKeyInclusive = true;
    *endKeyInclusive = true;

    size_t fieldNo = 0;

    // First, we skip over point intervals.
    for (; fieldNo < bounds.fields.size(); ++fieldNo) {
        const OrderedIntervalList& oil = bounds.fields[fieldNo];
        // A point interval requires just one interval...
        if (1 != oil.intervals.size()) {
            break;
        }
        if (!oil.intervals[0].isPoint()) {
            break;
        }
        // Since it's a point, start == end.
        startBob.append(oil.intervals[0].start);
        endBob.append(oil.intervals[0].end);
    }

    if (fieldNo >= bounds.fields.size()) {
        // All our intervals are points.  We count for all values of one field.
        *startKey = startBob.obj();
        *endKey = endBob.obj();
        return true;
    }

    // After point intervals we can have exactly one non-point interval.
    const OrderedIntervalList& nonPoint = bounds.fields[fieldNo];
    if (1 != nonPoint.intervals.size()) {
        return false;
    }

    // Add the non-point interval to our builder and set the inclusivity from it.
    startBob.append(nonPoint.intervals[0].start);
    *startKeyInclusive = nonPoint.intervals[0].startInclusive;
    endBob.append(nonPoint.intervals[0].end);
    *endKeyInclusive = nonPoint.intervals[0].endInclusive;

    ++fieldNo;

    // Get some "all values" intervals for comparison's sake.
    // TODO: make static?
    Interval minMax = IndexBoundsBuilder::allValues();
    Interval maxMin = minMax;
    maxMin.reverse();

    // And after the non-point interval we can have any number of "all values" intervals.
    for (; fieldNo < bounds.fields.size(); ++fieldNo) {
        const OrderedIntervalList& oil = bounds.fields[fieldNo];
        // "All Values" is just one point.
        if (1 != oil.intervals.size()) {
            break;
        }

        // Must be min->max or max->min.
        if (oil.intervals[0].equals(minMax)) {
            // As an example for the logic below, consider the index {a:1, b:1} and a count for
            // {a: {$gt: 2}}.  Our start key isn't inclusive (as it's $gt: 2) and looks like
            // {"":2} so far.  If we move to the key greater than {"":2, "": MaxKey} we will get
            // the first value of 'a' that is greater than 2.
            if (!*startKeyInclusive) {
                startBob.appendMaxKey("");
            } else {
                // In this case, consider the index {a:1, b:1} and a count for {a:{$gte: 2}}.
                // We want to look at all values where a is 2, so our start key is {"":2,
                // "":MinKey}.
                startBob.appendMinKey("");
            }

            // Same deal as above.  Consider the index {a:1, b:1} and a count for {a: {$lt: 2}}.
            // Our end key isn't inclusive as ($lt: 2) and looks like {"":2} so far.  We can't
            // look at any values where a is 2 so we have to stop at {"":2, "": MinKey} as
            // that's the smallest key where a is still 2.
            if (!*endKeyInclusive) {
                endBob.appendMinKey("");
            } else {
                endBob.appendMaxKey("");
            }
        } else if (oil.intervals[0].equals(maxMin)) {
            // The reasoning here is the same as above but with the directions reversed.
            if (!*startKeyInclusive) {
                startBob.appendMinKey("");
            } else {
                startBob.appendMaxKey("");
            }
            if (!*endKeyInclusive) {
                endBob.appendMaxKey("");
            } else {
                endBob.appendMinKey("");
            }
        } else {
            // No dice.
            break;
        }
    }

    if (fieldNo >= bounds.fields.size()) {
        *startKey = startBob.obj();
        *endKey = endBob.obj();
        return true;
    } else {
        return false;
    }
}

}  // namespace mongo
