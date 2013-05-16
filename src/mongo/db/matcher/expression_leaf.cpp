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

    void LeafMatchExpression::initPath( const StringData& path ) {
        _path = path;
        _fieldRef.parse( _path );
    }


    bool LeafMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        bool traversedArray = false;
        size_t idxPath = 0;
        BSONElement e = doc->getFieldDottedOrArray( _fieldRef, &idxPath, &traversedArray );

        if ( e.type() != Array || traversedArray ) {
            return matchesSingleElement( e );
        }

        BSONObjIterator i( e.Obj() );
        while ( i.more() ) {
            BSONElement x = i.next();
            bool found = false;
            if ( idxPath + 1 == _fieldRef.numParts() ) {
                found = matchesSingleElement( x );
            }
            else if ( x.isABSONObj() ) {
                string rest = _fieldRef.dottedField( idxPath+1 );
                BSONElement y = x.Obj().getFieldDotted( rest );

                if ( !y.eoo() )
                    found = matchesSingleElement( y );

                if ( !found && y.type() == Array ) {
                    // we iterate this array as well it seems
                    BSONObjIterator j( y.Obj() );
                    while( j.more() ) {
                        BSONElement sub = j.next();
                        found = matchesSingleElement( sub );
                        if ( found )
                            break;
                    }
                }

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

    bool ComparisonMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( other->matchType() != matchType() )
            return false;
        const ComparisonMatchExpression* realOther =
            static_cast<const ComparisonMatchExpression*>( other );

        return
            path() == realOther->path() &&
            _rhs.valuesEqual( realOther->_rhs );
    }


    Status ComparisonMatchExpression::init( const StringData& path, const BSONElement& rhs ) {
        initPath( path );
        _rhs = rhs;

        if ( rhs.eoo() ) {
            return Status( ErrorCodes::BadValue, "need a real operand" );
        }

        if ( rhs.type() == Undefined ) {
            return Status( ErrorCodes::BadValue, "cannot compare to undefined" );
        }

        switch ( matchType() ) {
        case NE:
            _allHaveToMatch = true;
            break;
        case LT:
        case LTE:
        case EQ:
        case GT:
        case GTE:
            _allHaveToMatch = false;
            break;
        default:
            return Status( ErrorCodes::BadValue, "bad match type for ComparisonMatchExpression" );
        }

        return Status::OK();
    }


    bool ComparisonMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        //log() << "\t ComparisonMatchExpression e: " << e << " _rhs: " << _rhs << "\n"
        //<< toString() << std::endl;

        if ( e.canonicalType() != _rhs.canonicalType() ) {
            // some special cases
            //  jstNULL and undefined are treated the same
            if ( e.canonicalType() + _rhs.canonicalType() == 5 ) {
                return matchType() == EQ || matchType() == LTE || matchType() == GTE;
            }

            if ( _rhs.type() == MaxKey || _rhs.type() == MinKey ) {
                return matchType() != EQ;
            }

            return _invertForNE( false );
        }

        if ( _rhs.type() == Array ) {
            if ( matchType() != EQ && matchType() != NE ) {
                return false;
            }
        }

        int x = compareElementValues( e, _rhs );

        //log() << "\t\t" << x << endl;

        switch ( matchType() ) {
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
        default:
            throw 1;
        }
        throw 1;
    }

    bool ComparisonMatchExpression::_invertForNE( bool normal ) const {
        if ( matchType() == NE )
            return !normal;
        return normal;
    }

    void ComparisonMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << " ";
        switch ( matchType() ) {
        case LT: debug << "$lt"; break;
        case LTE: debug << "$lte"; break;
        case EQ: debug << "=="; break;
        case GT: debug << "$gt"; break;
        case GTE: debug << "$gte"; break;
        case NE: debug << "$ne"; break;
        default: debug << " UNKNOWN - should be impossible"; break;
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

    bool RegexMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const RegexMatchExpression* realOther = static_cast<const RegexMatchExpression*>( other );
        return
            path() == realOther->path() &&
            _regex == realOther->_regex
            && _flags == realOther->_flags;
    }


    Status RegexMatchExpression::init( const StringData& path, const BSONElement& e ) {
        if ( e.type() != RegEx )
            return Status( ErrorCodes::BadValue, "regex not a regex" );
        return init( path, e.regex(), e.regexFlags() );
    }


    Status RegexMatchExpression::init( const StringData& path, const StringData& regex, const StringData& options ) {
        initPath( path );

        if ( regex.size() > MaxPatternSize ) {
            return Status( ErrorCodes::BadValue, "Regular expression is too long" );
        }

        _regex = regex.toString();
        _flags = options.toString();
        _re.reset( new pcrecpp::RE( _regex.c_str(), flags2options( _flags.c_str() ) ) );
        return Status::OK();
    }

    bool RegexMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        //log() << "RegexMatchExpression::matchesSingleElement _regex: " << _regex << " e: " << e << std::endl;
        switch (e.type()) {
        case String:
        case Symbol:
            // TODO
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

    void RegexMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << " regex /" << _regex << "/" << _flags << "\n";
    }

    // ---------

    Status ModMatchExpression::init( const StringData& path, int divisor, int remainder ) {
        initPath( path );
        if ( divisor == 0 )
            return Status( ErrorCodes::BadValue, "divisor cannot be 0" );
        _divisor = divisor;
        _remainder = remainder;
        return Status::OK();
    }

    bool ModMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( !e.isNumber() )
            return false;
        return e.numberLong() % _divisor == _remainder;
    }

    void ModMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << " mod " << _divisor << " % x == "  << _remainder << "\n";
    }

    bool ModMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const ModMatchExpression* realOther = static_cast<const ModMatchExpression*>( other );
        return
            path() == realOther->path() &&
            _divisor == realOther->_divisor &&
            _remainder == realOther->_remainder;
    }


    // ------------------

    Status ExistsMatchExpression::init( const StringData& path, bool exists ) {
        initPath( path );
        _exists = exists;
        return Status::OK();
    }

    bool ExistsMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( e.eoo() ) {
            return !_exists;
        }
        return _exists;
    }

    void ExistsMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << " exists: " << _exists << "\n";
    }

    bool ExistsMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const ExistsMatchExpression* realOther = static_cast<const ExistsMatchExpression*>( other );
        return path() == realOther->path() && _exists == realOther->_exists;
    }


    // ----

    Status TypeMatchExpression::init( const StringData& path, int type ) {
        _path = path;
        _type = type;
        return Status::OK();
    }

    bool TypeMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        return e.type() == _type;
    }

    bool TypeMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        return _matches( _path, doc, details );
    }

    bool TypeMatchExpression::_matches( const StringData& path,
                                        const MatchableDocument* doc,
                                        MatchDetails* details ) const {

        FieldRef fieldRef;
        fieldRef.parse( path );

        bool traversedArray = false;
        size_t idxPath = 0;
        BSONElement e = doc->getFieldDottedOrArray( fieldRef, &idxPath, &traversedArray );

        string rest = fieldRef.dottedField( idxPath + 1 );

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
                BSONMatchableDocument doc( x.Obj() );
                found = _matches( rest, &doc, details );
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

    void TypeMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " type: " << _type << "\n";
    }


    bool TypeMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const TypeMatchExpression* realOther = static_cast<const TypeMatchExpression*>( other );
        return _path == realOther->_path && _type == realOther->_type;
    }


    // --------

    ArrayFilterEntries::ArrayFilterEntries(){
        _hasNull = false;
        _hasEmptyArray = false;
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

        if ( e.type() == Array && e.Obj().isEmpty() )
            _hasEmptyArray = true;

        _equalities.insert( e );
        return Status::OK();
    }

    Status ArrayFilterEntries::addRegex( RegexMatchExpression* expr ) {
        _regexes.push_back( expr );
        return Status::OK();
    }

    bool ArrayFilterEntries::equivalent( const ArrayFilterEntries& other ) const {
        if ( _hasNull != other._hasNull )
            return false;

        if ( _regexes.size() != other._regexes.size() )
            return false;
        for ( unsigned i = 0; i < _regexes.size(); i++ )
            if ( !_regexes[i]->equivalent( other._regexes[i] ) )
                return false;

        return _equalities == other._equalities;
    }

    void ArrayFilterEntries::copyTo( ArrayFilterEntries& toFillIn ) const {
        toFillIn._hasNull = _hasNull;
        toFillIn._hasEmptyArray = _hasEmptyArray;
        toFillIn._equalities = _equalities;
        for ( unsigned i = 0; i < _regexes.size(); i++ )
            toFillIn._regexes.push_back( static_cast<RegexMatchExpression*>(_regexes[i]->shallowClone()) );
    }


    // -----------

    void InMatchExpression::init( const StringData& path ) {
        initPath( path );
        _allHaveToMatch = false;
    }


    bool InMatchExpression::matchesSingleElement( const BSONElement& e ) const {
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

    void InMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << " $in: TODO\n";
    }

    bool InMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;
        const InMatchExpression* realOther = static_cast<const InMatchExpression*>( other );
        return
            path() == realOther->path() &&
            _arrayEntries.equivalent( realOther->_arrayEntries );
    }

    LeafMatchExpression* InMatchExpression::shallowClone() const {
        InMatchExpression* next = new InMatchExpression();
        copyTo( next );
        return next;
    }

    void InMatchExpression::copyTo( InMatchExpression* toFillIn ) const {
        toFillIn->init( path() );
        _arrayEntries.copyTo( toFillIn->_arrayEntries );
    }

}


