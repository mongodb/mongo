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

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/index_bounds.h"

namespace mongo {

    /**
     * Translates expressions over fields into bounds on an index.
     */
    class IndexBoundsBuilder {
    public:
        /**
         * Return an O.I.L. with one interval goes from MinKey to MaxKey (or vice-versa depending on
         * the index direction).
         */
        static OrderedIntervalList allValuesForField(const BSONElement& elt);

        /**
         * Turn the LeafMatchExpression in 'expr' into a set of index bounds.  The field that 'expr'
         * is concerned with is indexed according to 'idxElt'.
         */
        static void translate(const LeafMatchExpression* expr, const BSONElement& idxElt,
                              OrderedIntervalList* oilOut, bool* exactOut);

    private:
        /**
         * Make a range interval from the provided object.
         * The object must have exactly two fields.  The first field is the start, the second the
         * end.
         * The two inclusive flags indicate whether or not the start/end fields are included in the
         * interval (closed interval if included, open if not).
         */
        static Interval makeRangeInterval(const BSONObj& obj, bool startInclusive,
                                          bool endInclusive);

        /**
         * Make a point interval from the provided object.
         * The object must have exactly one field which is the value of the point interval.
         */
        static Interval makePointInterval(const BSONObj& obj);

        /**
         * Since we have no BSONValue we must make an object that's a copy of a piece of another
         * object.
         */
        static BSONObj objFromElement(const BSONElement& elt);

        /**
         * Swap start/end in the provided interval.
         */
        static void reverseInterval(Interval* ival);
    };

}  // namespace mongo
