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
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    StatusWithMatchExpression expressionParserGeoCallbackReal( const char* name,
                                                               int type,
                                                               const BSONObj& section ) {
        if (BSONObj::opWITHIN == type || BSONObj::opGEO_INTERSECTS == type) {
            GeoQuery gq;
            if ( !gq.parseFrom( section ) )
                return StatusWithMatchExpression( ErrorCodes::BadValue, "bad geo query" );

            auto_ptr<GeoMatchExpression> e( new GeoMatchExpression() );
            Status s = e->init( name, gq );
            if ( !s.isOK() )
                return StatusWithMatchExpression( s );
            return StatusWithMatchExpression( e.release() );
        }
        else {
            NearQuery nq;
            if ( !nq.parseFrom( section ) )
                return StatusWithMatchExpression( ErrorCodes::BadValue, "bad geo near query" );

            auto_ptr<GeoNearMatchExpression> e( new GeoNearMatchExpression() );
            Status s = e->init( name, nq );
            if ( !s.isOK() )
                return StatusWithMatchExpression( s );
            return StatusWithMatchExpression( e.release() );
        }
    }

    MONGO_INITIALIZER( MatchExpressionParserGeo )( ::mongo::InitializerContext* context ) {
        expressionParserGeoCallback = expressionParserGeoCallbackReal;
        return Status::OK();
    }

}
