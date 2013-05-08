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

    class LeafMatchExpression : public MatchExpression {
    public:
        LeafMatchExpression( MatchType matchType ) : MatchExpression( LEAF, matchType ) {
            _allHaveToMatch = false;
        }

        virtual ~LeafMatchExpression(){}

        virtual LeafMatchExpression* shallowClone() const = 0;

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const;

        virtual bool matchesSingleElement( const BSONElement& e ) const = 0;

        virtual const StringData getPath() const { return _path; }

    protected:
        StringData _path;
        bool _allHaveToMatch;
    };

    // -----

    class ComparisonMatchExpression : public LeafMatchExpression {
    public:
        ComparisonMatchExpression( MatchType type ) : LeafMatchExpression( type ){}

        Status init( const StringData& path, const BSONElement& rhs );

        virtual ~ComparisonMatchExpression(){}

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

    protected:
        bool _invertForNE( bool normal ) const;

        BSONElement _rhs;
    };

    class EqualityMatchExpression : public ComparisonMatchExpression {
    public:
        EqualityMatchExpression() : ComparisonMatchExpression( EQ ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new EqualityMatchExpression();
            e->init( _path, _rhs  );
            return e;
        }
    };

    class LTEMatchExpression : public ComparisonMatchExpression {
    public:
        LTEMatchExpression() : ComparisonMatchExpression( LTE ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new LTEMatchExpression();
            e->init( _path, _rhs  );
            return e;
        }

    };

    class LTMatchExpression : public ComparisonMatchExpression {
    public:
        LTMatchExpression() : ComparisonMatchExpression( LT ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new LTMatchExpression();
            e->init( _path, _rhs  );
            return e;
        }

    };

    class GTMatchExpression : public ComparisonMatchExpression {
    public:
        GTMatchExpression() : ComparisonMatchExpression( GT ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new GTMatchExpression();
            e->init( _path, _rhs  );
            return e;
        }

    };

    class GTEMatchExpression : public ComparisonMatchExpression {
    public:
        GTEMatchExpression() : ComparisonMatchExpression( GTE ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new GTEMatchExpression();
            e->init( _path, _rhs  );
            return e;
        }

    };

    class NEMatchExpression : public ComparisonMatchExpression {
    public:
        NEMatchExpression() : ComparisonMatchExpression( NE ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new NEMatchExpression();
            e->init( _path, _rhs  );
            return e;
        }

    };


    class RegexMatchExpression : public LeafMatchExpression {
    public:
        /**
         * Maximum pattern size which pcre v8.3 can do matches correctly with
         * LINK_SIZE define macro set to 2 @ pcre's config.h (based on
         * experiments)
         */
        static const size_t MaxPatternSize = 32764;

        RegexMatchExpression() : LeafMatchExpression( REGEX ){}

        Status init( const StringData& path, const StringData& regex, const StringData& options );
        Status init( const StringData& path, const BSONElement& e );

        virtual LeafMatchExpression* shallowClone() const {
            RegexMatchExpression* e = new RegexMatchExpression();
            e->init( _path, _regex, _flags );
            return e;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

    private:
        std::string _regex;
        std::string _flags;
        boost::scoped_ptr<pcrecpp::RE> _re;
    };

    class ModMatchExpression : public LeafMatchExpression {
    public:
        ModMatchExpression() : LeafMatchExpression( MOD ){}

        Status init( const StringData& path, int divisor, int remainder );

        virtual LeafMatchExpression* shallowClone() const {
            ModMatchExpression* m = new ModMatchExpression();
            m->init( _path, _divisor, _remainder );
            return m;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

    private:
        int _divisor;
        int _remainder;
    };

    class ExistsMatchExpression : public LeafMatchExpression {
    public:
        ExistsMatchExpression() : LeafMatchExpression( EXISTS ){}

        Status init( const StringData& path, bool exists );

        virtual LeafMatchExpression* shallowClone() const {
            ExistsMatchExpression* e = new ExistsMatchExpression();
            e->init( _path, _exists );
            return e;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        // this is a terrible name, but trying not to use anythign we may really want
        bool rightSideBool() const { return _exists; }

    private:
        bool _exists;
    };

    class TypeMatchExpression : public MatchExpression {
    public:
        TypeMatchExpression() : MatchExpression( TYPE_CATEGORY, TYPE_OPERATOR ){}

        Status init( const StringData& path, int type );

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;
    private:
        bool _matches( const StringData& path,
                       const MatchableDocument* doc,
                       MatchDetails* details = 0 ) const;

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
        Status addRegex( RegexMatchExpression* expr );

        const BSONElementSet& equalities() const { return _equalities; }
        bool contains( const BSONElement& elem ) const { return _equalities.count(elem) > 0; }

        size_t numRegexes() const { return _regexes.size(); }
        RegexMatchExpression* regex( int idx ) const { return _regexes[idx]; }

        bool hasNull() const { return _hasNull; }
        bool singleNull() const { return size() == 1 && _hasNull; }
        int size() const { return _equalities.size() + _regexes.size(); }

        bool equivalent( const ArrayFilterEntries& other ) const;

        void copyTo( ArrayFilterEntries& toFillIn ) const;
    private:
        bool _hasNull; // if _equalities has a jstNULL element in it
        BSONElementSet _equalities;
        std::vector<RegexMatchExpression*> _regexes;
    };

    /**
     * query operator: $in
     */
    class InMatchExpression : public LeafMatchExpression {
    public:
        InMatchExpression() : LeafMatchExpression( MATCH_IN ){}
        void init( const StringData& path );

        virtual LeafMatchExpression* shallowClone() const;

        ArrayFilterEntries* getArrayFilterEntries() { return &_arrayEntries; }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        void copyTo( InMatchExpression* toFillIn ) const;

    private:
        ArrayFilterEntries _arrayEntries;
    };

    class NinMatchExpression : public LeafMatchExpression {
    public:
        NinMatchExpression() : LeafMatchExpression( NIN ){}
        void init( const StringData& path );

        virtual LeafMatchExpression* shallowClone() const;

        ArrayFilterEntries* getArrayFilterEntries() { return _in.getArrayFilterEntries(); }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

    private:
        InMatchExpression _in;
    };



}
