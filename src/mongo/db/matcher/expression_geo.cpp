// expression_geo.cpp

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

#include "mongo/pch.h"
#include "mongo/db/matcher/expression_geo.h"

namespace mongo {

    //
    // Geo queries we don't need an index to answer: geoWithin and geoIntersects
    //

    Status GeoMatchExpression::init( const StringData& path, const GeoQuery* query,
                                     const BSONObj& rawObj ) {
        _query.reset(query);
        _rawObj = rawObj;
        return initPath( path );
    }

    bool GeoMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( !e.isABSONObj())
            return false;

        GeometryContainer geometry;
        if ( !geometry.parseFrom( e.Obj() ) )
                return false;

        if (GeoQuery::WITHIN == _query->getPred()) {
            return _query->getGeometry().contains(geometry);
        }
        else {
            verify(GeoQuery::INTERSECT == _query->getPred());
            return _query->getGeometry().intersects(geometry);
        }
    }

    void GeoMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "GEO raw = " << _rawObj.toString();
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    void GeoMatchExpression::toBSON(BSONObjBuilder* out) const {
        out->appendElements(_rawObj);
    }

    bool GeoMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const GeoMatchExpression* realOther = static_cast<const GeoMatchExpression*>( other );

        if ( path() != realOther->path() )
            return false;

        // TODO:
        // return _query == realOther->_query;
        return false;
    }

    LeafMatchExpression* GeoMatchExpression::shallowClone() const {
        GeoMatchExpression* next = new GeoMatchExpression();
        next->init( path(), NULL, _rawObj);
        next->_query = _query;
        if (getTag()) {
            next->setTag(getTag()->clone());
        }
        return next;
    }

    //
    // Parse-only geo expressions: geoNear (formerly known as near).
    //

    Status GeoNearMatchExpression::init( const StringData& path, const NearQuery* query,
                                         const BSONObj& rawObj ) {
        _query.reset(query);
        _rawObj = rawObj;
        return initPath( path );
    }

    bool GeoNearMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        // See ops/update.cpp.
        // This node is removed by the query planner.  It's only ever called if we're getting an
        // elemMatchKey.
        return true;
    }

    void GeoNearMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "GEONEAR " << _query->toString();
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    void GeoNearMatchExpression::toBSON(BSONObjBuilder* out) const {
        out->appendElements(_rawObj);
    }

    bool GeoNearMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const GeoNearMatchExpression* realOther = static_cast<const GeoNearMatchExpression*>(other);

        if ( path() != realOther->path() )
            return false;

        // TODO:
        // return _query == realOther->_query;
        return false;
    }

    LeafMatchExpression* GeoNearMatchExpression::shallowClone() const {
        GeoNearMatchExpression* next = new GeoNearMatchExpression();
        next->init( path(), NULL, _rawObj );
        next->_query = _query;
        if (getTag()) {
            next->setTag(getTag()->clone());
        }
        return next;
    }

}
