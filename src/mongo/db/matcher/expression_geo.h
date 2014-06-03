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

#include "mongo/db/geo/geoquery.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    class GeoMatchExpression : public LeafMatchExpression {
    public:
        GeoMatchExpression() : LeafMatchExpression( GEO ){}
        virtual ~GeoMatchExpression(){}

        /**
         * Takes ownership of the passed-in GeoQuery.
         */
        Status init( const StringData& path, const GeoQuery* query, const BSONObj& rawObj );

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual void toBSON(BSONObjBuilder* out) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        virtual LeafMatchExpression* shallowClone() const;

        const GeoQuery& getGeoQuery() const { return *_query; }
        const BSONObj getRawObj() const { return _rawObj; }

    private:
        BSONObj _rawObj;
        // Share ownership of our query with all of our clones
        shared_ptr<const GeoQuery> _query;
    };

    class GeoNearMatchExpression : public LeafMatchExpression {
    public:
        GeoNearMatchExpression() : LeafMatchExpression( GEO_NEAR ){}
        virtual ~GeoNearMatchExpression(){}

        Status init( const StringData& path, const NearQuery& query, const BSONObj& rawObj );

        // This shouldn't be called and as such will crash.  GeoNear always requires an index.
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual void toBSON(BSONObjBuilder* out) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        virtual LeafMatchExpression* shallowClone() const;

        const NearQuery& getData() const { return _query; }
        const BSONObj getRawObj() const { return _rawObj; }
    private:
        NearQuery _query;
        BSONObj _rawObj;
    };

}  // namespace mongo
