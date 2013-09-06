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

#include "mongo/db/query/index_bounds_builder.h"

namespace mongo {

    // static
    OrderedIntervalList IndexBoundsBuilder::allValuesForField(const BSONElement& elt) {
        // ARGH, BSONValue would make this shorter.
        BSONObjBuilder bob;
        if (-1 == elt.number()) {
            // Index should go from MaxKey to MinKey as it's descending.
            bob.appendMaxKey("");
            bob.appendMinKey("");
        }
        else {
            // Index goes from MinKey to MaxKey as it's ascending.
            bob.appendMinKey("");
            bob.appendMaxKey("");
        }

        OrderedIntervalList oil(elt.fieldName());
        oil.intervals.push_back(makeRangeInterval(bob.obj(), true, true));
        return oil;
    }

    // static
    void IndexBoundsBuilder::translate(const LeafMatchExpression* expr, const BSONElement& idxElt,
                                       OrderedIntervalList* oilOut, bool* exactOut) {
        Interval interval;
        bool exact = false;

        if (MatchExpression::EQ == expr->matchType()) {
            const EqualityMatchExpression* node = static_cast<const EqualityMatchExpression*>(expr);
            // We have to copy the data out of the parse tree and stuff it into the index bounds.
            // BSONValue will be useful here.
            // XXX: This is wrong when idxElt is an array.
            // XXX: equality with arrays is weird, see queryutil.cpp:203
            BSONObj dataObj = objFromElement(node->getData());
            verify(dataObj.isOwned());
            interval = makePointInterval(dataObj);
            // TODO ALBERTO
            // XXX: This is false when idxElt is an array.
            exact = true;
        }
        else if (MatchExpression::LTE == expr->matchType()) {
            const LTEMatchExpression* node = static_cast<const LTEMatchExpression*>(expr);
            BSONObjBuilder bob;
            bob.appendMinKey("");
            bob.append(node->getData());
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, true, true);
            exact = true;
        }
        else if (MatchExpression::LT == expr->matchType()) {
            const LTMatchExpression* node = static_cast<const LTMatchExpression*>(expr);
            BSONObjBuilder bob;
            bob.appendMinKey("");
            bob.append(node->getData());
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, true, false);
            exact = true;
        }
        else if (MatchExpression::GT == expr->matchType()) {
            const GTMatchExpression* node = static_cast<const GTMatchExpression*>(expr);
            BSONObjBuilder bob;
            bob.append(node->getData());
            bob.appendMaxKey("");
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, false, true);
            exact = true;
        }
        else if (MatchExpression::GTE == expr->matchType()) {
            const GTEMatchExpression* node = static_cast<const GTEMatchExpression*>(expr);
            BSONObjBuilder bob;
            bob.append(node->getData());
            bob.appendMaxKey("");
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, true, true);
            exact = true;
        }
        else {
            verify(0);
        }

        if (-1 == idxElt.number()) {
            reverseInterval(&interval);
        }

        oilOut->intervals.push_back(interval);
        *exactOut = exact;
    }

    // static
    Interval IndexBoundsBuilder::makeRangeInterval(const BSONObj& obj, bool startInclusive,
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
    Interval IndexBoundsBuilder::makePointInterval(const BSONObj& obj) {
        Interval ret;
        ret._intervalData = obj;
        ret.startInclusive = ret.endInclusive = true;
        ret.start = ret.end = obj.firstElement();
        return ret;
    }

    // static
    BSONObj IndexBoundsBuilder::objFromElement(const BSONElement& elt) {
        BSONObjBuilder bob;
        bob.append(elt);
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

}  // namespace mongo
