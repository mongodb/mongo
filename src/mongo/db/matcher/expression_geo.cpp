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

    Status GeoMatchExpression::init( const StringData& path, const GeoQuery& query ) {
        _query = query;
        return initPath( path );
    }

    bool GeoMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( !e.isABSONObj())
            return false;

        GeometryContainer container;
        if ( !container.parseFrom( e.Obj() ) )
                return false;

        return _query.satisfiesPredicate( container );
    }

    void GeoMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "GEO";
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
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
        next->init( path(), _query );
        return next;
    }

    //
    // Parse-only geo expressions: geoNear (formerly known as near).
    //

    Status GeoNearMatchExpression::init( const StringData& path, const NearQuery& query ) {
        _query = query;
        return initPath( path );
    }

    bool GeoNearMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        // This shouldn't be called.
        verify(0);
        return false;
    }

    void GeoNearMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "GEONEAR";
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
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
        next->init( path(), _query );
        return next;
    }

}
