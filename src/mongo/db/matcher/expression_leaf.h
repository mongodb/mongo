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

#include <pcrecpp.h>

#include <boost/scoped_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/matcher/expression.h"

namespace mongo {

    /**
     * This file contains leaves in the parse tree that are not array-based.
     *
     * LeafMatchExpression: REGEX MOD EXISTS MATCH_IN
     * ComparisonMatchExpression: EQ LTE LT GT GTE
     * MatchExpression: TYPE_OPERATOR
     */

    /**
     * Many operators subclass from this:
     * REGEX, MOD, EXISTS, IN
     * Everything that inherits from ComparisonMatchExpression.
     */
    class LeafMatchExpression : public MatchExpression {
    public:
        LeafMatchExpression( MatchType matchType )
            : MatchExpression( matchType ) {
        }

        virtual ~LeafMatchExpression(){}

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const;

        virtual bool matchesSingleElement( const BSONElement& e ) const = 0;

        virtual const StringData path() const { return _path; }

        virtual void resetTag() { setTag(NULL); }

    protected:
        Status initPath( const StringData& path );

    private:
        StringData _path;
        ElementPath _elementPath;
    };

    /**
     * EQ, LTE, LT, GT, GTE subclass from ComparisonMatchExpression.
     */
    class ComparisonMatchExpression : public LeafMatchExpression {
    public:
        ComparisonMatchExpression( MatchType type ) : LeafMatchExpression( type ){}

        Status init( const StringData& path, const BSONElement& rhs );

        virtual ~ComparisonMatchExpression(){}

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual const BSONElement& getRHS() const { return _rhs; }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        const BSONElement& getData() const { return _rhs; }

    protected:
        BSONElement _rhs;
    };

    //
    // ComparisonMatchExpression inheritors
    //

    class EqualityMatchExpression : public ComparisonMatchExpression {
    public:
        EqualityMatchExpression() : ComparisonMatchExpression( EQ ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new EqualityMatchExpression();
            e->init( path(), _rhs  );
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }
    };

    class LTEMatchExpression : public ComparisonMatchExpression {
    public:
        LTEMatchExpression() : ComparisonMatchExpression( LTE ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new LTEMatchExpression();
            e->init( path(), _rhs  );
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

    };

    class LTMatchExpression : public ComparisonMatchExpression {
    public:
        LTMatchExpression() : ComparisonMatchExpression( LT ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new LTMatchExpression();
            e->init( path(), _rhs  );
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

    };

    class GTMatchExpression : public ComparisonMatchExpression {
    public:
        GTMatchExpression() : ComparisonMatchExpression( GT ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new GTMatchExpression();
            e->init( path(), _rhs  );
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

    };

    class GTEMatchExpression : public ComparisonMatchExpression {
    public:
        GTEMatchExpression() : ComparisonMatchExpression( GTE ){}
        virtual LeafMatchExpression* shallowClone() const {
            ComparisonMatchExpression* e = new GTEMatchExpression();
            e->init( path(), _rhs  );
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

    };

    //
    // LeafMatchExpression inheritors
    //

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
            e->init( path(), _regex, _flags );
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        const string& getString() const { return _regex; }
        const string& getFlags() const { return _flags; }

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
            m->init( path(), _divisor, _remainder );
            if ( getTag() ) {
                m->setTag(getTag()->clone());
            }
            return m;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        int getDivisor() const { return _divisor; }
        int getRemainder() const { return _remainder; }

    private:
        int _divisor;
        int _remainder;
    };

    class ExistsMatchExpression : public LeafMatchExpression {
    public:
        ExistsMatchExpression() : LeafMatchExpression( EXISTS ){}

        Status init( const StringData& path );

        virtual LeafMatchExpression* shallowClone() const {
            ExistsMatchExpression* e = new ExistsMatchExpression();
            e->init( path() );
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;
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
        bool hasEmptyArray() const { return _hasEmptyArray; }
        int size() const { return _equalities.size() + _regexes.size(); }

        bool equivalent( const ArrayFilterEntries& other ) const;

        void copyTo( ArrayFilterEntries& toFillIn ) const;

    private:
        bool _hasNull; // if _equalities has a jstNULL element in it
        bool _hasEmptyArray;
        BSONElementSet _equalities;
        std::vector<RegexMatchExpression*> _regexes;
    };

    /**
     * query operator: $in
     */
    class InMatchExpression : public LeafMatchExpression {
    public:
        InMatchExpression() : LeafMatchExpression( MATCH_IN ){}
        Status init( const StringData& path );

        virtual LeafMatchExpression* shallowClone() const;

        ArrayFilterEntries* getArrayFilterEntries() { return &_arrayEntries; }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        void copyTo( InMatchExpression* toFillIn ) const;

        const ArrayFilterEntries& getData() const { return _arrayEntries; }

    private:
        bool _matchesRealElement( const BSONElement& e ) const;
        ArrayFilterEntries _arrayEntries;
    };

    //
    // The odd duck out, TYPE_OPERATOR.
    //

    /**
     * Type has some odd semantics with arrays and as such it can't inherit from
     * LeafMatchExpression.
     */
    class TypeMatchExpression : public MatchExpression {
    public:
        TypeMatchExpression() : MatchExpression( TYPE_OPERATOR ){}

        Status init( const StringData& path, int type );

        virtual MatchExpression* shallowClone() const {
            TypeMatchExpression* e = new TypeMatchExpression();
            e->init(_path, _type);
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const;

        virtual void debugString( StringBuilder& debug, int level ) const;

        virtual bool equivalent( const MatchExpression* other ) const;

        /**
         * What is the type we're matching against?
         */
        int getData() const { return _type; }

        virtual const StringData path() const { return _path; }

        virtual void resetTag() { setTag(NULL); }

    private:
        bool _matches( const StringData& path,
                       const MatchableDocument* doc,
                       MatchDetails* details = 0 ) const;

        StringData _path;
        ElementPath _elementPath;
        int _type;
    };

}  // namespace mongo
