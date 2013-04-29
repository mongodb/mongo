// expression_leaf.h

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

#include <pcrecpp.h>

#include <boost/scoped_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/matcher/expression.h"

namespace mongo {

    class LeafExpression : public Expression {
    public:
        LeafExpression() {
            _allHaveToMatch = false;
        }

        virtual ~LeafExpression(){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;

        virtual bool matchesSingleElement( const BSONElement& e ) const = 0;
    protected:
        StringData _path;
        bool _allHaveToMatch;
    };

    // -----

    class ComparisonExpression : public LeafExpression {
    public:
        enum Type { LTE, LT, EQ, GT, GTE, NE };

        Status init( const StringData& path, Type type, const BSONElement& rhs );

        virtual ~ComparisonExpression(){}

        virtual Type getType() const { return _type; }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;
    protected:
        bool _invertForNE( bool normal ) const;

    private:
        Type _type;
        BSONElement _rhs;
    };

    class RegexExpression : public LeafExpression {
    public:
        /**
         * Maximum pattern size which pcre v8.3 can do matches correctly with
         * LINK_SIZE define macro set to 2 @ pcre's config.h (based on
         * experiments)
         */
        static const size_t MaxPatternSize = 32764;

        Status init( const StringData& path, const StringData& regex, const StringData& options );
        Status init( const StringData& path, const BSONElement& e );

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        std::string _regex;
        std::string _flags;
        boost::scoped_ptr<pcrecpp::RE> _re;
    };

    class ModExpression : public LeafExpression {
    public:
        Status init( const StringData& path, int divisor, int remainder );

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        int _divisor;
        int _remainder;
    };

    class ExistsExpression : public LeafExpression {
    public:
        Status init( const StringData& path, bool exists );

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        bool _exists;
    };

    class TypeExpression : public Expression {
    public:
        Status init( const StringData& path, int type );

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        bool _matches( const StringData& path, const BSONObj& doc, MatchDetails* details = 0 ) const;

        StringData _path;
        int _type;
    };

    /**
     * INTERNAL
     * terrible name
     * holds the entries of an $in or $all
     * either scalars or regex
     */
    class ArrayFilterEntries {
        MONGO_DISALLOW_COPYING( ArrayFilterEntries );
    public:
        ArrayFilterEntries();
        ~ArrayFilterEntries();

        Status addEquality( const BSONElement& e );
        Status addRegex( RegexExpression* expr );

        const BSONElementSet& equalities() const { return _equalities; }
        bool contains( const BSONElement& elem ) const { return _equalities.count(elem) > 0; }

        size_t numRegexes() const { return _regexes.size(); }
        RegexExpression* regex( int idx ) const { return _regexes[idx]; }

        bool hasNull() const { return _hasNull; }
        bool singleNull() const { return size() == 1 && _hasNull; }
        int size() const { return _equalities.size() + _regexes.size(); }

    private:
        bool _hasNull; // if _equalities has a jstNULL element in it
        BSONElementSet _equalities;
        std::vector<RegexExpression*> _regexes;
    };


    /**
     * query operator: $in
     */
    class InExpression : public LeafExpression {
    public:
        void init( const StringData& path );
        ArrayFilterEntries* getArrayFilterEntries() { return &_arrayEntries; }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        ArrayFilterEntries _arrayEntries;
    };

    class NinExpression : public LeafExpression {
    public:
        void init( const StringData& path );
        ArrayFilterEntries* getArrayFilterEntries() { return _in.getArrayFilterEntries(); }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;
    private:
        InExpression _in;
    };



}
