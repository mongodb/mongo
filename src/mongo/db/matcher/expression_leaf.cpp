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

#include "mongo/db/matcher/expression_leaf.h"

#include "mongo/bson/bsonobjiterator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/path.h"
#include "mongo/util/log.h"

namespace mongo {

    Status LeafMatchExpression::initPath( const StringData& path ) {
        _path = path;
        return _elementPath.init( _path );
    }


    bool LeafMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        MatchableDocument::IteratorHolder cursor( doc, &_elementPath );
        while ( cursor->more() ) {
            ElementIterator::Context e = cursor->next();
            if ( !matchesSingleElement( e.element() ) )
                continue;
            if ( details && details->needRecord() && !e.arrayOffset().eoo() ) {
                details->setElemMatchKey( e.arrayOffset().fieldName() );
            }
            return true;
        }
        return false;
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
        _rhs = rhs;

        if ( rhs.eoo() ) {
            return Status( ErrorCodes::BadValue, "need a real operand" );
        }

        if ( rhs.type() == Undefined ) {
            return Status( ErrorCodes::BadValue, "cannot compare to undefined" );
        }

        switch ( matchType() ) {
        case LT:
        case LTE:
        case EQ:
        case GT:
        case GTE:
            break;
        default:
            return Status( ErrorCodes::BadValue, "bad match type for ComparisonMatchExpression" );
        }

        return initPath( path );
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

            return false;
        }

        if ( _rhs.type() == Array ) {
            if ( matchType() != EQ ) {
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
        default:
            fassertFailed( 16828 );
        }
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
        default: debug << " UNKNOWN - should be impossible"; break;
        }
        debug << " " << _rhs.toString( false );

        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }

        debug << "\n";
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
        if ( regex.size() > MaxPatternSize ) {
            return Status( ErrorCodes::BadValue, "Regular expression is too long" );
        }

        _regex = regex.toString();
        _flags = options.toString();
        _re.reset( new pcrecpp::RE( _regex.c_str(), flags2options( _flags.c_str() ) ) );

        return initPath( path );
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
        debug << path() << " regex /" << _regex << "/" << _flags;

        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    // ---------

    Status ModMatchExpression::init( const StringData& path, int divisor, int remainder ) {
        if ( divisor == 0 )
            return Status( ErrorCodes::BadValue, "divisor cannot be 0" );
        _divisor = divisor;
        _remainder = remainder;
        return initPath( path );
    }

    bool ModMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( !e.isNumber() )
            return false;
        return e.numberLong() % _divisor == _remainder;
    }

    void ModMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << " mod " << _divisor << " % x == "  << _remainder;
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
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

    Status ExistsMatchExpression::init( const StringData& path ) {
        return initPath( path );
    }

    bool ExistsMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        return !e.eoo();
    }

    void ExistsMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << " exists";
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    bool ExistsMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const ExistsMatchExpression* realOther = static_cast<const ExistsMatchExpression*>( other );
        return path() == realOther->path();
    }


    // ----

    Status TypeMatchExpression::init( const StringData& path, int type ) {
        _path = path;
        _type = type;
        return _elementPath.init( _path );
    }

    bool TypeMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        return e.type() == _type;
    }

    bool TypeMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        MatchableDocument::IteratorHolder cursor( doc, &_elementPath );
        while ( cursor->more() ) {
            ElementIterator::Context e = cursor->next();
            if ( e.outerArray() )
                continue;

            if ( !matchesSingleElement( e.element() ) )
                continue;
            if ( details && details->needRecord() && !e.arrayOffset().eoo() ) {
                details->setElemMatchKey( e.arrayOffset().fieldName() );
            }
            return true;
        }
        return false;
    }

    void TypeMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " type: " << _type;
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
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

    Status InMatchExpression::init( const StringData& path ) {
        return initPath( path );
    }

    bool InMatchExpression::_matchesRealElement( const BSONElement& e ) const {
        if ( _arrayEntries.contains( e ) )
            return true;

        for ( unsigned i = 0; i < _arrayEntries.numRegexes(); i++ ) {
            if ( _arrayEntries.regex(i)->matchesSingleElement( e ) )
                return true;
        }

        return false;
    }

    bool InMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( _arrayEntries.hasNull() && e.eoo() )
            return true;

        if ( _matchesRealElement( e ) )
            return true;

        /*
        if ( e.type() == Array ) {
            BSONObjIterator i( e.Obj() );
            while ( i.more() ) {
                BSONElement sub = i.next();
                if ( _matchesRealElement( sub ) )
                    return true;
            }
        }
        */

        return false;
    }

    void InMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << ";$in: TODO ";
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
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
        if ( getTag() ) {
            next->setTag(getTag()->clone());
        }
        return next;
    }

    void InMatchExpression::copyTo( InMatchExpression* toFillIn ) const {
        toFillIn->init( path() );
        _arrayEntries.copyTo( toFillIn->_arrayEntries );
    }

}


