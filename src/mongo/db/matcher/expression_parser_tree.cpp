// expression_parser_tree.cpp

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

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsonobjiterator.h"
#include "mongo/bson/bson-inl.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    Status MatchExpressionParser::_parseTreeList( const BSONObj& arr, ListOfMatchExpression* out ) {
        if ( arr.isEmpty() )
            return Status( ErrorCodes::BadValue,
                           "$and/$or/$nor must be a nonempty array" );

        BSONObjIterator i( arr );
        while ( i.more() ) {
            BSONElement e = i.next();

            if ( e.type() != Object )
                return Status( ErrorCodes::BadValue,
                               "$or/$and/$nor entries need to be full objects" );

            StatusWithMatchExpression sub = _parse( e.Obj(), false );
            if ( !sub.isOK() )
                return sub.getStatus();

            out->add( sub.getValue() );
        }
        return Status::OK();
    }

    StatusWithMatchExpression MatchExpressionParser::_parseNot( const char* name,
                                                      const BSONElement& e ) {
        if ( e.type() == RegEx ) {
            StatusWithMatchExpression s = _parseRegexElement( name, e );
            if ( !s.isOK() )
                return s;
            std::auto_ptr<NotMatchExpression> n( new NotMatchExpression() );
            Status s2 = n->init( s.getValue() );
            if ( !s2.isOK() )
                return StatusWithMatchExpression( s2 );
            return StatusWithMatchExpression( n.release() );
        }

        if ( e.type() != Object )
            return StatusWithMatchExpression( ErrorCodes::BadValue, "$not needs a regex or a document" );

        BSONObj notObject = e.Obj();
        if ( notObject.isEmpty() )
            return StatusWithMatchExpression( ErrorCodes::BadValue, "$not cannot be empty" );

        std::auto_ptr<AndMatchExpression> theAnd( new AndMatchExpression() );
        Status s = _parseSub( name, notObject, theAnd.get() );
        if ( !s.isOK() )
            return StatusWithMatchExpression( s );

        // TODO: this seems arbitrary?
        // tested in jstests/not2.js
        for ( unsigned i = 0; i < theAnd->numChildren(); i++ )
            if ( theAnd->getChild(i)->matchType() == MatchExpression::REGEX )
                return StatusWithMatchExpression( ErrorCodes::BadValue, "$not cannot have a regex" );

        std::auto_ptr<NotMatchExpression> theNot( new NotMatchExpression() );
        s = theNot->init( theAnd.release() );
        if ( !s.isOK() )
            return StatusWithMatchExpression( s );

        return StatusWithMatchExpression( theNot.release() );
    }

}
