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
 */

#include "mongo/pch.h"
#include "mongo/db/matcher/expression_geo.h"

namespace mongo {

    Status GeoMatchExpression::init( const StringData& path, const GeoQuery& query ) {
        _path = path;
        _query = query;
        return Status::OK();
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
        debug << "GEO\n";
    }

    bool GeoMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const GeoMatchExpression* realOther = static_cast<const GeoMatchExpression*>( other );

        if ( _path != realOther->_path )
            return false;

        // TODO:
        // return _query == realOther->_query;
        return false;
    }

    LeafMatchExpression* GeoMatchExpression::shallowClone() const {
        GeoMatchExpression* next = new GeoMatchExpression();
        next->init( _path, _query );
        return next;
    }

}
