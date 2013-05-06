// expression_tree.h

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

#include "mongo/db/matcher/expression.h"

#include <boost/scoped_ptr.hpp>

/**
 * this contains all Expessions that define the structure of the tree
 * they do not look at the structure of the documents themselves, just combine other things
 */
namespace mongo {

    class ListOfMatchExpression : public MatchExpression {
    public:
        ListOfMatchExpression( MatchType type ) : MatchExpression( TREE, type ){}
        virtual ~ListOfMatchExpression();

        /**
         * @param e - I take ownership
         */
        void add( MatchExpression* e );

        /**
         * clears all the thingsd we own, and does NOT delete
         * someone else has taken ownership
         */
        void clearAndRelease() { _expressions.clear(); }

        virtual size_t numChildren() const { return _expressions.size(); }
        virtual const MatchExpression* getChild( size_t i ) const { return _expressions[i]; }

        bool equivalent( const MatchExpression* other ) const;

    protected:
        void _debugList( StringBuilder& debug, int level ) const;

    private:
        std::vector< MatchExpression* > _expressions;
    };

    class AndMatchExpression : public ListOfMatchExpression {
    public:
        AndMatchExpression() : ListOfMatchExpression( AND ){}
        virtual ~AndMatchExpression(){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;
    };

    class OrMatchExpression : public ListOfMatchExpression {
    public:
        OrMatchExpression() : ListOfMatchExpression( OR ){}
        virtual ~OrMatchExpression(){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;
    };

    class NorMatchExpression : public ListOfMatchExpression {
    public:
        NorMatchExpression() : ListOfMatchExpression( NOR ){}
        virtual ~NorMatchExpression(){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;
    };

    class NotMatchExpression : public MatchExpression {
    public:
        NotMatchExpression() : MatchExpression( TREE, NOT ){}
        /**
         * @param exp - I own it, and will delete
         */
        virtual Status init( MatchExpression* exp ) {
            _exp.reset( exp );
            return Status::OK();
        }

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const {
            return !_exp->matches( doc, NULL );
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const {
            return !_exp->matchesSingleElement( e );
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        bool equivalent( const MatchExpression* other ) const;

        virtual size_t numChildren() const { return 1; }
        virtual MatchExpression* getChild( size_t i ) const { return _exp.get(); }


    private:
        boost::scoped_ptr<MatchExpression> _exp;
    };

}
