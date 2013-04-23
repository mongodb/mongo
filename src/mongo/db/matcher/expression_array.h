// expression_array.h

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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {
    /**
     * this SHOULD extend from ArrayMatchingExpression
     * the only reason it can't is

     > db.foo.insert( { x : 5 } )
     > db.foo.insert( { x : [5] } )
     > db.foo.find( { x : { $all : [ 5 ] } } )
     { "_id" : ObjectId("5162b5c3f98a76ce1e70ed0c"), "x" : 5 }
     { "_id" : ObjectId("5162b5c5f98a76ce1e70ed0d"), "x" : [ 5 ] }

     * the { x : 5}  doc should NOT match
     *
     */
    class AllExpression : public Expression {
    public:
        Status init( const StringData& path );
        ArrayFilterEntries* getArrayFilterEntries() { return &_arrayEntries; }

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        bool _match( const BSONElementSet& all ) const;

        StringData _path;
        ArrayFilterEntries _arrayEntries;
    };


    class ArrayMatchingExpression : public Expression {
    public:
        virtual ~ArrayMatchingExpression(){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details ) const;

        /**
         * @param e - has to be an array.  calls matchesArray with e as an array
         */
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual bool matchesArray( const BSONObj& anArray, MatchDetails* details ) const = 0;

    protected:
        StringData _path;
    };


    class ElemMatchObjectExpression : public ArrayMatchingExpression {
    public:
        Status init( const StringData& path, const Expression* sub );

        bool matchesArray( const BSONObj& anArray, MatchDetails* details ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        boost::scoped_ptr<const Expression> _sub;
    };

    class ElemMatchValueExpression : public ArrayMatchingExpression {
    public:
        virtual ~ElemMatchValueExpression();

        Status init( const StringData& path );
        Status init( const StringData& path, const Expression* sub );
        void add( const Expression* sub );

        bool matchesArray( const BSONObj& anArray, MatchDetails* details ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        bool _arrayElementMatchesAll( const BSONElement& e ) const;

        std::vector< const Expression* > _subs;
    };


    /**
     * i'm suprised this isn't a regular AllExpression
     */
    class AllElemMatchOp : public Expression {
    public:
        virtual ~AllElemMatchOp();

        Status init( const StringData& path );
        void add( const ArrayMatchingExpression* expr );

        virtual bool matches( const BSONObj& doc, MatchDetails* details ) const;

        /**
         * @param e has to be an array
         */
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        bool _allMatch( const BSONObj& anArray ) const;

        StringData _path;
        std::vector< const ArrayMatchingExpression* > _list;
    };

    class SizeExpression : public ArrayMatchingExpression {
    public:
        Status init( const StringData& path, int size );

        virtual bool matchesArray( const BSONObj& anArray, MatchDetails* details ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        int _size; // >= 0 real, < 0, nothing will match
    };

}
