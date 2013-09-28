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
#include "mongo/db/query/indexability.h"
#include "mongo/db/index/expression_index.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    // static
    void IndexBoundsBuilder::allValuesForField(const BSONElement& elt, OrderedIntervalList* out) {
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

        out->name = elt.fieldName();
        out->intervals.push_back(makeRangeInterval(bob.obj(), true, true));
    }

    Interval IndexBoundsBuilder::allValues() {
        BSONObjBuilder bob;
        bob.appendMinKey("");
        bob.appendMaxKey("");
        return makeRangeInterval(bob.obj(), true, true);
    }

    // static
    void IndexBoundsBuilder::translate(const MatchExpression* expr, const BSONElement& elt,
                                       OrderedIntervalList* oilOut, bool* exactOut) {
        int direction = (elt.numberInt() >= 0) ? 1 : -1;

        Interval interval;
        bool exact = false;
        oilOut->name = elt.fieldName();

        bool isHashed = false;
        if (mongoutils::str::equals("hashed", elt.valuestrsafe())) {
            isHashed = true;
        }

        if (isHashed) {
            verify(MatchExpression::EQ == expr->matchType()
                   || MatchExpression::MATCH_IN == expr->matchType());
        }

        if (MatchExpression::EQ == expr->matchType()) {
            const EqualityMatchExpression* node =
                static_cast<const EqualityMatchExpression*>(expr);

            // We have to copy the data out of the parse tree and stuff it into the index
            // bounds.  BSONValue will be useful here.
            BSONObj dataObj;

            if (isHashed) {
                dataObj = ExpressionMapping::hash(node->getData());
            }
            else {
                dataObj = objFromElement(node->getData());
            }

            // UNITTEST 11738048
            if (Array == dataObj.firstElement().type()) {
                // XXX: build better bounds
                warning() << "building lazy bounds for " << expr->toString() << endl;
                interval = allValues();
                exact = false;
            }
            else {
                verify(dataObj.isOwned());
                interval = makePointInterval(dataObj);
                if (dataObj.firstElement().isNull()) {
                    exact = false;
                }
                else if (isHashed) {
                    exact = false;
                }
                else {
                    exact = true;
                }
            }
        }
        else if (MatchExpression::LTE == expr->matchType()) {
            const LTEMatchExpression* node = static_cast<const LTEMatchExpression*>(expr);
            BSONElement dataElt = node->getData();
            BSONObjBuilder bob;
            bob.appendMinForType("", dataElt.type());
            bob.append(dataElt);
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, true, true);
            // XXX: only exact if not (null or array)
            exact = true;
        }
        else if (MatchExpression::LT == expr->matchType()) {
            const LTMatchExpression* node = static_cast<const LTMatchExpression*>(expr);
            BSONElement dataElt = node->getData();
            BSONObjBuilder bob;
            bob.appendMinForType("", dataElt.type());
            bob.append(dataElt);
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, true, false);
            // XXX: only exact if not (null or array)
            exact = true;
        }
        else if (MatchExpression::GT == expr->matchType()) {
            const GTMatchExpression* node = static_cast<const GTMatchExpression*>(expr);
            BSONElement dataElt = node->getData();
            BSONObjBuilder bob;
            bob.append(node->getData());
            bob.appendMaxForType("", dataElt.type());
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, false, true);
            // XXX: only exact if not (null or array)
            exact = true;
        }
        else if (MatchExpression::GTE == expr->matchType()) {
            const GTEMatchExpression* node = static_cast<const GTEMatchExpression*>(expr);
            BSONElement dataElt = node->getData();

            BSONObjBuilder bob;
            bob.append(dataElt);
            bob.appendMaxForType("", dataElt.type());
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, true, true);
            // XXX: only exact if not (null or array)
            exact = true;
        }
        else if (MatchExpression::REGEX == expr->matchType()) {
            warning() << "building lazy bounds for " << expr->toString() << endl;
            interval = allValues();
            exact = false;
        }
        else if (MatchExpression::MOD == expr->matchType()) {
            BSONObjBuilder bob;
            bob.appendMinForType("", NumberDouble);
            bob.appendMaxForType("", NumberDouble);
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, true, true);
            exact = false;
        }
        else if (MatchExpression::MATCH_IN == expr->matchType()) {
            warning() << "building lazy bounds for " << expr->toString() << endl;
            interval = allValues();
            exact = false;
        }
        else if (MatchExpression::TYPE_OPERATOR == expr->matchType()) {
            const TypeMatchExpression* tme = static_cast<const TypeMatchExpression*>(expr);
            BSONObjBuilder bob;
            bob.appendMinForType("", tme->getData());
            bob.appendMaxForType("", tme->getData());
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            interval = makeRangeInterval(dataObj, true, true);
            exact = false;
        }
        else if (MatchExpression::MATCH_IN == expr->matchType()) {
            warning() << "building lazy bounds for " << expr->toString() << endl;
            interval = allValues();
            exact = false;
        }
        else {
            warning() << "Planner error, trying to build bounds for expr " << expr->toString() << endl;
            verify(0);
        }

        if (-1 == direction) {
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
