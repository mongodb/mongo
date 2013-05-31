// expression_tree.cpp

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

#include "mongo/db/matcher/expression_tree.h"

#include "mongo/bson/bsonobjiterator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/util/log.h"

namespace mongo {

    ListOfMatchExpression::~ListOfMatchExpression() {
        for ( unsigned i = 0; i < _expressions.size(); i++ )
            delete _expressions[i];
        _expressions.clear();
    }

    void ListOfMatchExpression::add( MatchExpression* e ) {
        verify( e );
        _expressions.push_back( e );
    }


    void ListOfMatchExpression::_debugList( StringBuilder& debug, int level ) const {
        for ( unsigned i = 0; i < _expressions.size(); i++ )
            _expressions[i]->debugString( debug, level + 1 );
    }

    bool ListOfMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const ListOfMatchExpression* realOther = static_cast<const ListOfMatchExpression*>( other );

        if ( _expressions.size() != realOther->_expressions.size() )
            return false;

        // TOOD: order doesn't matter
        for ( unsigned i = 0; i < _expressions.size(); i++ )
            if ( !_expressions[i]->equivalent( realOther->_expressions[i] ) )
                return false;

        return true;
    }

    // -----

    bool AndMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        for ( size_t i = 0; i < numChildren(); i++ ) {
            if ( !getChild(i)->matches( doc, details ) ) {
                if ( details )
                    details->resetOutput();
                return false;
            }
        }
        return true;
    }

    bool AndMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        for ( size_t i = 0; i < numChildren(); i++ ) {
            if ( !getChild(i)->matchesSingleElement( e ) ) {
                return false;
            }
        }
        return true;
    }


    void AndMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$and\n";
        _debugList( debug, level );
    }

    // -----

    bool OrMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        for ( size_t i = 0; i < numChildren(); i++ ) {
            if ( getChild(i)->matches( doc, NULL ) ) {
                return true;
            }
        }
        return false;
    }

    bool OrMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        for ( size_t i = 0; i < numChildren(); i++ ) {
            if ( getChild(i)->matchesSingleElement( e ) ) {
                return true;
            }
        }
        return false;
    }


    void OrMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$or\n";
        _debugList( debug, level );
    }

    // ----

    bool NorMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        for ( size_t i = 0; i < numChildren(); i++ ) {
            if ( getChild(i)->matches( doc, NULL ) ) {
                return false;
            }
        }
        return true;
    }

    bool NorMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        for ( size_t i = 0; i < numChildren(); i++ ) {
            if ( getChild(i)->matchesSingleElement( e ) ) {
                return false;
            }
        }
        return true;
    }

    void NorMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$nor\n";
        _debugList( debug, level );
    }

    // -------

    void NotMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$not\n";
        _exp->debugString( debug, level + 1 );
    }

    bool NotMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        return _exp->equivalent( other->getChild(0) );
    }

}
