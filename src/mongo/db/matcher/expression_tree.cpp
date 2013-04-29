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

    ListOfExpression::~ListOfExpression() {
        for ( unsigned i = 0; i < _expressions.size(); i++ )
            delete _expressions[i];
        _expressions.clear();
    }

    void ListOfExpression::add( Expression* e ) {
        verify( e );
        _expressions.push_back( e );
    }


    void ListOfExpression::_debugList( StringBuilder& debug, int level ) const {
        for ( unsigned i = 0; i < _expressions.size(); i++ )
            _expressions[i]->debugString( debug, level + 1 );
    }

    // -----

    bool AndExpression::matches( const BSONObj& doc, MatchDetails* details ) const {
        for ( size_t i = 0; i < size(); i++ ) {
            if ( !get(i)->matches( doc, details ) ) {
                if ( details )
                    details->resetOutput();
                return false;
            }
        }
        return true;
    }

    bool AndExpression::matchesSingleElement( const BSONElement& e ) const {
        for ( size_t i = 0; i < size(); i++ ) {
            if ( !get(i)->matchesSingleElement( e ) ) {
                return false;
            }
        }
        return true;
    }


    void AndExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$and\n";
        _debugList( debug, level );
    }

    // -----

    bool OrExpression::matches( const BSONObj& doc, MatchDetails* details ) const {
        for ( size_t i = 0; i < size(); i++ ) {
            if ( get(i)->matches( doc, NULL ) ) {
                return true;
            }
        }
        return false;
    }

    bool OrExpression::matchesSingleElement( const BSONElement& e ) const {
        for ( size_t i = 0; i < size(); i++ ) {
            if ( get(i)->matchesSingleElement( e ) ) {
                return true;
            }
        }
        return false;
    }


    void OrExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$or\n";
        _debugList( debug, level );
    }

    // ----

    bool NorExpression::matches( const BSONObj& doc, MatchDetails* details ) const {
        for ( size_t i = 0; i < size(); i++ ) {
            if ( get(i)->matches( doc, NULL ) ) {
                return false;
            }
        }
        return true;
    }

    bool NorExpression::matchesSingleElement( const BSONElement& e ) const {
        for ( size_t i = 0; i < size(); i++ ) {
            if ( get(i)->matchesSingleElement( e ) ) {
                return false;
            }
        }
        return true;
    }

    void NorExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$nor\n";
        _debugList( debug, level );
    }

    // -------

    void NotExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$not\n";
        _exp->debugString( debug, level + 1 );
    }

}
