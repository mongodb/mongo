// expression_geo.h

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


#pragma once

#include "mongo/db/geo/geoquery.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    class GeoMatchExpression : public LeafMatchExpression {
    public:
        GeoMatchExpression() : LeafMatchExpression( GEO ){}
        virtual ~GeoMatchExpression(){}

        Status init( const StringData& path, const GeoQuery& query );

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        virtual LeafMatchExpression* shallowClone() const;

    private:
        GeoQuery _query;
    };

}
