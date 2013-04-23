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
 */

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsonobjiterator.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    Status ExpressionParser::_parseTreeList( const BSONObj& arr, ListOfExpression* out ) {
        BSONObjIterator i( arr );
        while ( i.more() ) {
            BSONElement e = i.next();

            if ( e.type() != Object )
                return Status( ErrorCodes::BadValue,
                               "$or/$and/$nor entries need to be full objects" );

            StatusWithExpression sub = parse( e.Obj() );
            if ( !sub.isOK() )
                return sub.getStatus();

            out->add( sub.getValue() );
        }
        return Status::OK();
    }

    StatusWithExpression ExpressionParser::_parseNot( const char* name,
                                                      const BSONElement& e ) {
        if ( e.type() == RegEx ) {
            StatusWithExpression s = _parseRegexElement( name, e );
            if ( !s.isOK() )
                return s;
            std::auto_ptr<NotExpression> n( new NotExpression() );
            Status s2 = n->init( s.getValue() );
            if ( !s2.isOK() )
                return StatusWithExpression( s2 );
            return StatusWithExpression( n.release() );
        }

        if ( e.type() != Object )
            return StatusWithExpression( ErrorCodes::BadValue, "$not needs a regex or a document" );

        std::auto_ptr<AndExpression> theAnd( new AndExpression() );
        Status s = _parseSub( name, e.Obj(), theAnd.get() );
        if ( !s.isOK() )
            return StatusWithExpression( s );

        std::auto_ptr<NotExpression> theNot( new NotExpression() );
        s = theNot->init( theAnd.release() );
        if ( !s.isOK() )
            return StatusWithExpression( s );

        return StatusWithExpression( theNot.release() );
    }

}
