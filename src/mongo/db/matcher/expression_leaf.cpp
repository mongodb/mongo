// expression_leaf.cpp

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

#include "mongo/db/matcher/expression_leaf.h"

#include "mongo/bson/bsonobjiterator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression_internal.h"
#include "mongo/util/log.h"

namespace mongo {

    bool LeafExpression::matches( const BSONObj& doc, MatchDetails* details ) const {
        //log() << "e doc: " << doc << " path: " << _path << std::endl;

        FieldRef path;
        path.parse(_path);

        bool traversedArray = false;
        int32_t idxPath = 0;
        BSONElement e = getFieldDottedOrArray( doc, path, &idxPath, &traversedArray );

        string rest = pathToString( path, idxPath+1 );

        if ( e.type() != Array || traversedArray ) {
            return matchesSingleElement( e );
        }

        BSONObjIterator i( e.Obj() );
        while ( i.more() ) {
            BSONElement x = i.next();
            bool found = false;
            if ( rest.size() == 0 ) {
                found = matchesSingleElement( x );
            }
            else if ( x.isABSONObj() ) {
                BSONElement y = x.Obj().getField( rest );
                found = matchesSingleElement( y );
            }

            if ( found ) {
                if ( !_allHaveToMatch ) {
                    if ( details && details->needRecord() ) {
                        // this block doesn't have to be inside the _allHaveToMatch handler
                        // but this matches the old semantics
                        details->setElemMatchKey( x.fieldName() );
                    }
                    return true;
                }
            }
            else if ( _allHaveToMatch ) {
                return false;
            }
        }

        return matchesSingleElement( e );
    }

    // -------------

    Status ComparisonExpression::init( const StringData& path, Type type, const BSONElement& rhs ) {
        _path = path;
        _type = type;
        _rhs = rhs;
        if ( rhs.eoo() ) {
            return Status( ErrorCodes::BadValue, "need a real operand" );
        }

        _allHaveToMatch = _type == NE;

        return Status::OK();
    }


    bool ComparisonExpression::matchesSingleElement( const BSONElement& e ) const {
        //log() << "\t ComparisonExpression e: " << e << " _rhs: " << _rhs << std::endl;

        if ( e.canonicalType() != _rhs.canonicalType() ) {
            // some special cases
            //  jstNULL and undefined are treated the same
            if ( e.canonicalType() + _rhs.canonicalType() == 5 ) {
                return _type == EQ || _type == LTE || _type == GTE;
            }
            return _invertForNE( false );
        }

        if ( _rhs.type() == Array ) {
            if ( _type != EQ && _type != NE ) {
                return false;
            }
        }

        int x = compareElementValues( e, _rhs );

        switch ( _type ) {
        case LT:
            return x < 0;
        case LTE:
            return x <= 0;
        case EQ:
            return x == 0;
        case GT:
            return x > 0;
        case GTE:
            return x >= 0;
        case NE:
            return x != 0;
        }
        throw 1;
    }

    bool ComparisonExpression::_invertForNE( bool normal ) const {
        if ( _type == NE )
            return !normal;
        return normal;
    }

    void ComparisonExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " ";
        switch ( _type ) {
        case LT: debug << "$lt"; break;
        case LTE: debug << "$lte"; break;
        case EQ: debug << "=="; break;
        case GT: debug << "$gt"; break;
        case GTE: debug << "$gte"; break;
        case NE: debug << "$ne"; break;
        }
        debug << " " << _rhs.toString( false ) << "\n";
    }

    // ---------------

    // TODO: move
    inline pcrecpp::RE_Options flags2options(const char* flags) {
        pcrecpp::RE_Options options;
        options.set_utf8(true);
        while ( flags && *flags ) {
            if ( *flags == 'i' )
                options.set_caseless(true);
            else if ( *flags == 'm' )
                options.set_multiline(true);
            else if ( *flags == 'x' )
                options.set_extended(true);
            else if ( *flags == 's' )
                options.set_dotall(true);
            flags++;
        }
        return options;
    }

    Status RegexExpression::init( const StringData& path, const BSONElement& e ) {
        if ( e.type() != RegEx )
            return Status( ErrorCodes::BadValue, "regex not a regex" );
        return init( path, e.regex(), e.regexFlags() );
    }


    Status RegexExpression::init( const StringData& path, const StringData& regex, const StringData& options ) {
        _path = path;

        if ( regex.size() > MaxPatternSize ) {
            return Status( ErrorCodes::BadValue, "Regular expression is too long" );
        }

        _regex = regex.toString();
        _flags = options.toString();
        _re.reset( new pcrecpp::RE( _regex.c_str(), flags2options( _flags.c_str() ) ) );
        return Status::OK();
    }

    bool RegexExpression::matchesSingleElement( const BSONElement& e ) const {
        //log() << "RegexExpression::matchesSingleElement _regex: " << _regex << " e: " << e << std::endl;
        switch (e.type()) {
        case String:
        case Symbol:
            //if (rm._prefix.empty())
                return _re->PartialMatch(e.valuestr());
                //else
                //return !strncmp(e.valuestr(), rm._prefix.c_str(), rm._prefix.size());
        case RegEx:
            return _regex == e.regex() && _flags == e.regexFlags();
        default:
            return false;
        }
    }

    void RegexExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " regex /" << _regex << "/" << _flags << "\n";
    }

    // ---------

    Status ModExpression::init( const StringData& path, int divisor, int remainder ) {
        _path = path;
        if ( divisor == 0 )
            return Status( ErrorCodes::BadValue, "divisor cannot be 0" );
        _divisor = divisor;
        _remainder = remainder;
        return Status::OK();
    }

    bool ModExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( !e.isNumber() )
            return false;
        return e.numberLong() % _divisor == _remainder;
    }

    void ModExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " mod " << _divisor << " % x == "  << _remainder << "\n";
    }


    // ------------------

    Status ExistsExpression::init( const StringData& path, bool exists ) {
        _path = path;
        _exists = exists;
        return Status::OK();
    }

    bool ExistsExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( e.eoo() ) {
            return !_exists;
        }
        return _exists;
    }

    void ExistsExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " exists: " << _exists << "\n";
    }

    // ----

    Status TypeExpression::init( const StringData& path, int type ) {
        _path = path;
        _type = type;
        return Status::OK();
    }

    bool TypeExpression::matchesSingleElement( const BSONElement& e ) const {
        return e.type() == _type;
    }

    bool TypeExpression::matches( const BSONObj& doc, MatchDetails* details ) const {
        return _matches( _path, doc, details );
    }

    bool TypeExpression::_matches( const StringData& path, const BSONObj& doc, MatchDetails* details ) const {

        FieldRef pathRef;
        pathRef.parse(path);

        bool traversedArray = false;
        int32_t idxPath = 0;
        BSONElement e = getFieldDottedOrArray( doc, pathRef, &idxPath, &traversedArray );

        string rest = pathToString( pathRef, idxPath+1 );

        if ( e.type() != Array ) {
            return matchesSingleElement( e );
        }

        BSONObjIterator i( e.Obj() );
        while ( i.more() ) {
            BSONElement x = i.next();
            bool found = false;
            if ( rest.size() == 0 ) {
                found = matchesSingleElement( x );
            }
            else if ( x.isABSONObj() ) {
                found = _matches( rest, x.Obj(), details );
            }

            if ( found ) {
                if ( details && details->needRecord() ) {
                    // this block doesn't have to be inside the _allHaveToMatch handler
                    // but this matches the old semantics
                    details->setElemMatchKey( x.fieldName() );
                }
                return true;
            }
        }

        return false;
    }

    void TypeExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " type: " << _type << "\n";
    }


    // --------

    ArrayFilterEntries::ArrayFilterEntries(){
        _hasNull = false;
    }

    ArrayFilterEntries::~ArrayFilterEntries() {
        for ( unsigned i = 0; i < _regexes.size(); i++ )
            delete _regexes[i];
        _regexes.clear();
    }

    Status ArrayFilterEntries::addEquality( const BSONElement& e ) {
        if ( e.isABSONObj() ) {
            if ( e.Obj().firstElement().fieldName()[0] == '$' )
                return Status( ErrorCodes::BadValue, "cannot next $ under $in" );
        }

        if ( e.type() == RegEx )
            return Status( ErrorCodes::BadValue, "ArrayFilterEntries equality cannot be a regex" );

        if ( e.type() == jstNULL ) {
            _hasNull = true;
        }

        _equalities.insert( e );
        return Status::OK();
    }

    Status ArrayFilterEntries::addRegex( RegexExpression* expr ) {
        _regexes.push_back( expr );
        return Status::OK();
    }

    // -----------

    void InExpression::init( const StringData& path ) {
        _path = path;
        _allHaveToMatch = false;
    }


    bool InExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( _arrayEntries.hasNull() && e.eoo() )
            return true;

        if ( _arrayEntries.contains( e ) )
            return true;

        for ( unsigned i = 0; i < _arrayEntries.numRegexes(); i++ ) {
            if ( _arrayEntries.regex(i)->matchesSingleElement( e ) )
                return true;
        }

        return false;
    }

    void InExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " $in: TODO\n";
    }


    // ----------

    void NinExpression::init( const StringData& path ) {
        _path = path;
        _allHaveToMatch = true;
    }


    bool NinExpression::matchesSingleElement( const BSONElement& e ) const {
        return !_in.matchesSingleElement( e );
    }


    void NinExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " $nin: TODO\n";
    }

}


