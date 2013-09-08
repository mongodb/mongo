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

#include "mongo/db/matcher/expression.h"

#include <boost/scoped_ptr.hpp>

/**
 * this contains all Expessions that define the structure of the tree
 * they do not look at the structure of the documents themselves, just combine other things
 */
namespace mongo {

    class ListOfMatchExpression : public MatchExpression {
    public:
        ListOfMatchExpression( MatchType type ) : MatchExpression( type ){}
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
        virtual MatchExpression* getChild( size_t i ) const { return _expressions[i]; }

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

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual MatchExpression* shallowClone() const {
            AndMatchExpression* self = new AndMatchExpression();
            for (size_t i = 0; i < numChildren(); ++i) {
                self->add(getChild(i)->shallowClone());
            }
            if ( getTag() ) {
                self->setTag(getTag()->clone());
            }
            return self;
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual void resetTag() {
            setTag(NULL);
            for (size_t i = 0; i < numChildren(); ++i) {
                getChild(i)->resetTag();
            }
        }
    };

    class OrMatchExpression : public ListOfMatchExpression {
    public:
        OrMatchExpression() : ListOfMatchExpression( OR ){}
        virtual ~OrMatchExpression(){}

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual MatchExpression* shallowClone() const {
            OrMatchExpression* self = new OrMatchExpression();
            for (size_t i = 0; i < numChildren(); ++i) {
                self->add(getChild(i)->shallowClone());
            }
            if ( getTag() ) {
                self->setTag(getTag()->clone());
            }
            return self;
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual void resetTag() {
            setTag(NULL);
            for (size_t i = 0; i < numChildren(); ++i) {
                getChild(i)->resetTag();
            }
        }
    };

    class NorMatchExpression : public ListOfMatchExpression {
    public:
        NorMatchExpression() : ListOfMatchExpression( NOR ){}
        virtual ~NorMatchExpression(){}

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const;
        virtual bool matchesSingleElement( const BSONElement& e ) const;

        virtual MatchExpression* shallowClone() const {
            NorMatchExpression* self = new NorMatchExpression();
            for (size_t i = 0; i < numChildren(); ++i) {
                self->add(getChild(i)->shallowClone());
            }
            if ( getTag() ) {
                self->setTag(getTag()->clone());
            }
            return self;
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual void resetTag() {
            setTag(NULL);
            for (size_t i = 0; i < numChildren(); ++i) {
                getChild(i)->resetTag();
            }
        }
    };

    class NotMatchExpression : public MatchExpression {
    public:
        NotMatchExpression() : MatchExpression( NOT ){}
        NotMatchExpression( MatchExpression* e ) : MatchExpression( NOT ), _exp( e ){}
        /**
         * @param exp - I own it, and will delete
         */
        virtual Status init( MatchExpression* exp ) {
            _exp.reset( exp );
            return Status::OK();
        }

        virtual MatchExpression* shallowClone() const {
            NotMatchExpression* self = new NotMatchExpression();
            MatchExpression* child = _exp->shallowClone();
            self->init(child);
            if ( getTag() ) {
                self->setTag(getTag()->clone());
            }
            return self;
        }

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const {
            return !_exp->matches( doc, NULL );
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const {
            return !_exp->matchesSingleElement( e );
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        bool equivalent( const MatchExpression* other ) const;

        virtual size_t numChildren() const { return 1; }
        virtual MatchExpression* getChild( size_t i ) const { return _exp.get(); }

        virtual void resetTag() {
            setTag( NULL );
            _exp->resetTag();
        }

    private:
        boost::scoped_ptr<MatchExpression> _exp;
    };

}
