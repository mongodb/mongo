// expression_parser.cpp

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

#include "mongo/bson/bsonmisc.h"
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

    StatusWithExpression ExpressionParser::_parseComparison( const char* name,
                                                                ComparisonExpression::Type cmp,
                                                                const BSONElement& e ) {
        std::auto_ptr<ComparisonExpression> temp( new ComparisonExpression() );

        Status s = temp->init( name, cmp, e );
        if ( !s.isOK() )
            return StatusWithExpression(s);

        return StatusWithExpression( temp.release() );
    }

    StatusWithExpression ExpressionParser::_parseSubField( const char* name,
                                                           const BSONElement& e ) {

        // TODO: these should move to getGtLtOp, or its replacement

        if ( mongoutils::str::equals( "$eq", e.fieldName() ) )
            return _parseComparison( name, ComparisonExpression::EQ, e );

        if ( mongoutils::str::equals( "$not", e.fieldName() ) ) {
            return _parseNot( name, e );
        }


        int x = e.getGtLtOp(-1);
        switch ( x ) {
        case -1:
            return StatusWithExpression( ErrorCodes::BadValue,
                                         mongoutils::str::stream() << "unknown operator: "
                                         << e.fieldName() );
        case BSONObj::LT:
            return _parseComparison( name, ComparisonExpression::LT, e );
        case BSONObj::LTE:
            return _parseComparison( name, ComparisonExpression::LTE, e );
        case BSONObj::GT:
            return _parseComparison( name, ComparisonExpression::GT, e );
        case BSONObj::GTE:
            return _parseComparison( name, ComparisonExpression::GTE, e );
        case BSONObj::NE:
            return _parseComparison( name, ComparisonExpression::NE, e );
        case BSONObj::Equality:
            return _parseComparison( name, ComparisonExpression::EQ, e );

        case BSONObj::opIN: {
            if ( e.type() != Array )
                return StatusWithExpression( ErrorCodes::BadValue, "$in needs an array" );
            std::auto_ptr<InExpression> temp( new InExpression() );
            temp->init( name );
            Status s = _parseArrayFilterEntries( temp->getArrayFilterEntries(), e.Obj() );
            if ( !s.isOK() )
                return StatusWithExpression( s );
            return StatusWithExpression( temp.release() );
        }

        case BSONObj::NIN: {
            if ( e.type() != Array )
                return StatusWithExpression( ErrorCodes::BadValue, "$nin needs an array" );
            std::auto_ptr<NinExpression> temp( new NinExpression() );
            temp->init( name );
            Status s = _parseArrayFilterEntries( temp->getArrayFilterEntries(), e.Obj() );
            if ( !s.isOK() )
                return StatusWithExpression( s );
            return StatusWithExpression( temp.release() );
        }

        case BSONObj::opSIZE: {
            int size = 0;
            if ( e.type() == String ) {
                // matching old odd semantics
                size = 0;
            }
            else if ( e.type() == NumberInt || e.type() == NumberLong ) {
                size = e.numberInt();
            }
            else if ( e.type() == NumberDouble ) {
                if ( e.numberInt() == e.numberDouble() ) {
                    size = e.numberInt();
                }
                else {
                    // old semantcs require exact numeric match
                    // so [1,2] != 1 or 2
                    size = -1;
                }
            }
            else {
                return StatusWithExpression( ErrorCodes::BadValue, "$size needs a number" );
            }

            std::auto_ptr<SizeExpression> temp( new SizeExpression() );
            Status s = temp->init( name, size );
            if ( !s.isOK() )
                return StatusWithExpression( s );
            return StatusWithExpression( temp.release() );
        }

        case BSONObj::opEXISTS: {
            if ( e.eoo() )
                return StatusWithExpression( ErrorCodes::BadValue, "$exists can't be eoo" );
            std::auto_ptr<ExistsExpression> temp( new ExistsExpression() );
            Status s = temp->init( name, e.trueValue() );
            if ( !s.isOK() )
                return StatusWithExpression( s );
            return StatusWithExpression( temp.release() );
        }

        case BSONObj::opTYPE: {
            if ( !e.isNumber() )
                return StatusWithExpression( ErrorCodes::BadValue, "$type has to be a number" );
            int type = e.numberInt();
            if ( e.type() != NumberInt && type != e.number() )
                type = -1;
            std::auto_ptr<TypeExpression> temp( new TypeExpression() );
            Status s = temp->init( name, type );
            if ( !s.isOK() )
                return StatusWithExpression( s );
            return StatusWithExpression( temp.release() );
        }


        case BSONObj::opMOD:
            return _parseMOD( name, e );

        case BSONObj::opOPTIONS:
            return StatusWithExpression( ErrorCodes::BadValue, "$options has to be after a $regex" );

        case BSONObj::opELEM_MATCH:
            return _parseElemMatch( name, e );

        case BSONObj::opALL:
            return _parseAll( name, e );

        default:
            return StatusWithExpression( ErrorCodes::BadValue, "not done" );
        }

    }

    StatusWithExpression ExpressionParser::parse( const BSONObj& obj ) {

        std::auto_ptr<AndExpression> root( new AndExpression() );

        BSONObjIterator i( obj );
        while ( i.more() ){

            BSONElement e = i.next();
            if ( e.fieldName()[0] == '$' ) {
                const char * rest = e.fieldName() + 1;

                // TODO: optimize if block?
                if ( mongoutils::str::equals( "or", rest ) ) {
                    if ( e.type() != Array )
                        return StatusWithExpression( ErrorCodes::BadValue,
                                                     "$or needs an array" );
                    std::auto_ptr<OrExpression> temp( new OrExpression() );
                    Status s = _parseTreeList( e.Obj(), temp.get() );
                    if ( !s.isOK() )
                        return StatusWithExpression( s );
                    root->add( temp.release() );
                }
                else if ( mongoutils::str::equals( "and", rest ) ) {
                    if ( e.type() != Array )
                        return StatusWithExpression( ErrorCodes::BadValue,
                                                     "and needs an array" );
                    std::auto_ptr<AndExpression> temp( new AndExpression() );
                    Status s = _parseTreeList( e.Obj(), temp.get() );
                    if ( !s.isOK() )
                        return StatusWithExpression( s );
                    root->add( temp.release() );
                }
                else if ( mongoutils::str::equals( "nor", rest ) ) {
                    if ( e.type() != Array )
                        return StatusWithExpression( ErrorCodes::BadValue,
                                                     "and needs an array" );
                    std::auto_ptr<NorExpression> temp( new NorExpression() );
                    Status s = _parseTreeList( e.Obj(), temp.get() );
                    if ( !s.isOK() )
                        return StatusWithExpression( s );
                    root->add( temp.release() );
                }
                else {
                    return StatusWithExpression( ErrorCodes::BadValue,
                                                 mongoutils::str::stream()
                                                 << "unkown operator: "
                                                 << e.fieldName() );
                }

                continue;
            }

            if ( e.type() == Object && e.Obj().firstElement().fieldName()[0] == '$' ) {
                Status s = _parseSub( e.fieldName(), e.Obj(), root.get() );
                if ( !s.isOK() )
                    return StatusWithExpression( s );
                continue;
            }

            if ( e.type() == RegEx ) {
                StatusWithExpression result = _parseRegexElement( e.fieldName(), e );
                if ( !result.isOK() )
                    return result;
                root->add( result.getValue() );
                continue;
            }

            std::auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
            Status s = eq->init( e.fieldName(), ComparisonExpression::EQ, e );
            if ( !s.isOK() )
                return StatusWithExpression( s );

            root->add( eq.release() );
        }

        return StatusWithExpression( root.release() );
    }

    Status ExpressionParser::_parseSub( const char* name,
                                        const BSONObj& sub,
                                        AndExpression* root ) {

        bool first = true;

        BSONObjIterator j( sub );
        while ( j.more() ) {
            BSONElement deep = j.next();

            int op = deep.getGtLtOp();
            if ( op == BSONObj::opREGEX ) {
                if ( !first )
                    return Status( ErrorCodes::BadValue, "$regex has to be first" );

                StatusWithExpression s = _parseRegexDocument( name, sub );
                if ( !s.isOK() )
                    return s.getStatus();
                root->add( s.getValue() );
                return Status::OK();
            }

            StatusWithExpression s = _parseSubField( name, deep );
            if ( !s.isOK() )
                return s.getStatus();

            root->add( s.getValue() );
            first = false;
        }
        return Status::OK();
    }



    StatusWithExpression ExpressionParser::_parseMOD( const char* name,
                                                      const BSONElement& e ) {

        if ( e.type() != Array )
            return StatusWithExpression( ErrorCodes::BadValue, "malformed mod, needs to be an array" );

        BSONObjIterator i( e.Obj() );

        if ( !i.more() )
            return StatusWithExpression( ErrorCodes::BadValue, "malformed mod, not enough elements" );
        BSONElement d = i.next();
        if ( !d.isNumber() )
            return StatusWithExpression( ErrorCodes::BadValue, "malformed mod, divisor not a number" );

        if ( !i.more() )
            return StatusWithExpression( ErrorCodes::BadValue, "malformed mod, not enough elements" );
        BSONElement r = i.next();
        if ( !d.isNumber() )
            return StatusWithExpression( ErrorCodes::BadValue, "malformed mod, remainder not a number" );

        if ( i.more() )
            return StatusWithExpression( ErrorCodes::BadValue, "malformed mod, too many elements" );

        std::auto_ptr<ModExpression> temp( new ModExpression() );
        Status s = temp->init( name, d.numberInt(), r.numberInt() );
        if ( !s.isOK() )
            return StatusWithExpression( s );
        return StatusWithExpression( temp.release() );
    }

    StatusWithExpression ExpressionParser::_parseRegexElement( const char* name,
                                                               const BSONElement& e ) {
        if ( e.type() != RegEx )
            return StatusWithExpression( ErrorCodes::BadValue, "not a regex" );

        std::auto_ptr<RegexExpression> temp( new RegexExpression() );
        Status s = temp->init( name, e.regex(), e.regexFlags() );
        if ( !s.isOK() )
            return StatusWithExpression( s );
        return StatusWithExpression( temp.release() );
    }

    StatusWithExpression ExpressionParser::_parseRegexDocument( const char* name,
                                                                const BSONObj& doc ) {
        string regex;
        string regexOptions;

        BSONObjIterator i( doc );
        while ( i.more() ) {
            BSONElement e = i.next();
            switch ( e.getGtLtOp() ) {
            case BSONObj::opREGEX:
                if ( e.type() != String )
                    return StatusWithExpression( ErrorCodes::BadValue,
                                                 "$regex has to be a string" );
                regex = e.String();
                break;
            case BSONObj::opOPTIONS:
                if ( e.type() != String )
                    return StatusWithExpression( ErrorCodes::BadValue,
                                                 "$options has to be a string" );
                regexOptions = e.String();
                break;
            default:
                return StatusWithExpression( ErrorCodes::BadValue,
                                             mongoutils::str::stream()
                                             << "bad $regex doc option: " << e.fieldName() );
            }

        }

        std::auto_ptr<RegexExpression> temp( new RegexExpression() );
        Status s = temp->init( name, regex, regexOptions );
        if ( !s.isOK() )
            return StatusWithExpression( s );
        return StatusWithExpression( temp.release() );

    }

    Status ExpressionParser::_parseArrayFilterEntries( ArrayFilterEntries* entries,
                                                       const BSONObj& theArray ) {


        BSONObjIterator i( theArray );
        while ( i.more() ) {
            BSONElement e = i.next();

            if ( e.type() == RegEx ) {
                std::auto_ptr<RegexExpression> r( new RegexExpression() );
                Status s = r->init( "", e );
                if ( !s.isOK() )
                    return s;
                s =  entries->addRegex( r.release() );
                if ( !s.isOK() )
                    return s;
            }
            else {
                Status s = entries->addEquality( e );
                if ( !s.isOK() )
                    return s;
            }
        }
        return Status::OK();

    }

    StatusWithExpression ExpressionParser::_parseElemMatch( const char* name,
                                                            const BSONElement& e ) {
        if ( e.type() != Object )
            return StatusWithExpression( ErrorCodes::BadValue, "$elemMatch needs an Object" );

        BSONObj obj = e.Obj();
        if ( obj.firstElement().fieldName()[0] == '$' ) {
            // value case

            AndExpression theAnd;
            Status s = _parseSub( "", obj, &theAnd );
            if ( !s.isOK() )
                return StatusWithExpression( s );

            std::auto_ptr<ElemMatchValueExpression> temp( new ElemMatchValueExpression() );
            s = temp->init( name );
            if ( !s.isOK() )
                return StatusWithExpression( s );

            for ( size_t i = 0; i < theAnd.size(); i++ ) {
                temp->add( theAnd.get( i ) );
            }
            theAnd.clearAndRelease();

            return StatusWithExpression( temp.release() );
        }

        // object case

        StatusWithExpression sub = parse( obj );
        if ( !sub.isOK() )
            return sub;

        std::auto_ptr<ElemMatchObjectExpression> temp( new ElemMatchObjectExpression() );
        Status status = temp->init( name, sub.getValue() );
        if ( !status.isOK() )
            return StatusWithExpression( status );

        return StatusWithExpression( temp.release() );
    }

    StatusWithExpression ExpressionParser::_parseAll( const char* name,
                                                      const BSONElement& e ) {
        if ( e.type() != Array )
            return StatusWithExpression( ErrorCodes::BadValue, "$all needs an array" );

        BSONObj arr = e.Obj();
        if ( arr.firstElement().type() == Object &&
             mongoutils::str::equals( "$elemMatch",
                                      arr.firstElement().Obj().firstElement().fieldName() ) ) {
            // $all : [ { $elemMatch : {} } ... ]

            std::auto_ptr<AllElemMatchOp> temp( new AllElemMatchOp() );
            Status s = temp->init( name );
            if ( !s.isOK() )
                return StatusWithExpression( s );

            BSONObjIterator i( arr );
            while ( i.more() ) {
                BSONElement hopefullyElemMatchElemennt = i.next();

                if ( hopefullyElemMatchElemennt.type() != Object ) {
                    // $all : [ { $elemMatch : ... }, 5 ]
                    return StatusWithExpression( ErrorCodes::BadValue,
                                                 "$all/$elemMatch has to be consistent" );
                }

                BSONObj hopefullyElemMatchObj = hopefullyElemMatchElemennt.Obj();
                if ( !mongoutils::str::equals( "$elemMatch",
                                               hopefullyElemMatchObj.firstElement().fieldName() ) ) {
                    // $all : [ { $elemMatch : ... }, { x : 5 } ]
                    return StatusWithExpression( ErrorCodes::BadValue,
                                                 "$all/$elemMatch has to be consistent" );
                }

                StatusWithExpression inner = _parseElemMatch( "", hopefullyElemMatchObj.firstElement() );
                if ( !inner.isOK() )
                    return inner;
                temp->add( static_cast<ArrayMatchingExpression*>( inner.getValue() ) );
            }

            return StatusWithExpression( temp.release() );
        }

        std::auto_ptr<AllExpression> temp( new AllExpression() );
        Status s = temp->init( name );
        if ( !s.isOK() )
            return StatusWithExpression( s );

        s = _parseArrayFilterEntries( temp->getArrayFilterEntries(), arr );
        if ( !s.isOK() )
            return StatusWithExpression( s );

        return StatusWithExpression( temp.release() );
    }


}
