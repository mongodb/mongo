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

    class ListOfExpression : public Expression {
    public:
        virtual ~ListOfExpression();

        /**
         * @param e - I take ownership
         */
        void add( Expression* e );

        /**
         * clears all the thingsd we own, and does NOT delete
         * someone else has taken ownership
         */
        void clearAndRelease() { _expressions.clear(); }

        size_t size() const { return _expressions.size(); }
        Expression* get( size_t i ) const { return _expressions[i]; }

    protected:
        void _debugList( StringBuilder& debug, int level ) const;

    private:
        std::vector< Expression* > _expressions;
    };

    class AndExpression : public ListOfExpression {
    public:
        virtual ~AndExpression(){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;
    };

    class OrExpression : public ListOfExpression {
    public:
        virtual ~OrExpression(){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;
    };

    class NorExpression : public ListOfExpression {
    public:
        virtual ~NorExpression(){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;
    };

    class NotExpression : public Expression {
    public:
        /**
         * @param exp - I own it, and will delete
         */
        virtual Status init( Expression* exp ) {
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
    private:
        boost::scoped_ptr<Expression> _exp;
    };

}
