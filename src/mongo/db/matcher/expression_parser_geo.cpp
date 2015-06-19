// expression_parser_geo.cpp

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

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/base/init.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using stdx::make_unique;

StatusWithMatchExpression expressionParserGeoCallbackReal(const char* name,
                                                          int type,
                                                          const BSONObj& section) {
    if (BSONObj::opWITHIN == type || BSONObj::opGEO_INTERSECTS == type) {
        unique_ptr<GeoExpression> gq = make_unique<GeoExpression>(name);
        Status parseStatus = gq->parseFrom(section);

        if (!parseStatus.isOK())
            return StatusWithMatchExpression(parseStatus);

        unique_ptr<GeoMatchExpression> e = make_unique<GeoMatchExpression>();

        // Until the index layer accepts non-BSON predicates, or special indices are moved into
        // stages, we have to clean up the raw object so it can be passed down to the index
        // layer.
        BSONObjBuilder bob;
        bob.append(name, section);
        Status s = e->init(name, gq.release(), bob.obj());
        if (!s.isOK())
            return StatusWithMatchExpression(s);
        return {std::move(e)};
    } else {
        verify(BSONObj::opNEAR == type);
        unique_ptr<GeoNearExpression> nq = make_unique<GeoNearExpression>(name);
        Status s = nq->parseFrom(section);
        if (!s.isOK()) {
            return StatusWithMatchExpression(s);
        }
        unique_ptr<GeoNearMatchExpression> e = make_unique<GeoNearMatchExpression>();
        // Until the index layer accepts non-BSON predicates, or special indices are moved into
        // stages, we have to clean up the raw object so it can be passed down to the index
        // layer.
        BSONObjBuilder bob;
        bob.append(name, section);
        s = e->init(name, nq.release(), bob.obj());
        if (!s.isOK())
            return StatusWithMatchExpression(s);
        return {std::move(e)};
    }
}

MONGO_INITIALIZER(MatchExpressionParserGeo)(::mongo::InitializerContext* context) {
    expressionParserGeoCallback = expressionParserGeoCallbackReal;
    return Status::OK();
}
}
