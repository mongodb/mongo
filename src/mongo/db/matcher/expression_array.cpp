// expression_array.cpp

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

#include "mongo/db/matcher/expression_array.h"

#include "mongo/bson/bsonobjiterator.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression_internal.h"
#include "mongo/util/log.h"

namespace mongo {


    // ----------

    Status AllMatchExpression::init( const StringData& path ) {
        _path = path;
        return Status::OK();
    }

    bool AllMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        FieldRef path;
        path.parse(_path);

        bool traversedArray = false;
        int32_t idxPath = 0;
        BSONElement e = doc->getFieldDottedOrArray( path, &idxPath, &traversedArray );

        string rest = pathToString( path, idxPath+1 );

        if ( e.type() != Array || traversedArray || rest.size() == 0 ) {
            return matchesSingleElement( e );
        }

        BSONElementSet all;

        BSONObjIterator i( e.Obj() );
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( ! e.isABSONObj() )
                continue;

            e.Obj().getFieldsDotted( rest, all );
        }

        return _match( all );
    }

    bool AllMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( _arrayEntries.size() == 0 )
            return false;

        if ( e.eoo() )
            return _arrayEntries.singleNull();

        BSONElementSet all;

        if ( e.type() == Array ) {
            BSONObjIterator i( e.Obj() );
            while ( i.more() ) {
                all.insert( i.next() );
            }
        }
        else {
            // this is the part i want to remove
            all.insert( e );
        }

        return _match( all );
    }

    bool AllMatchExpression::_match( const BSONElementSet& all ) const {

        if ( all.size() == 0 )
            return _arrayEntries.singleNull();

        const BSONElementSet& equalities = _arrayEntries.equalities();
        for ( BSONElementSet::const_iterator i = equalities.begin(); i != equalities.end(); ++i ) {
            BSONElement foo = *i;
            if ( all.count( foo ) == 0 )
                return false;
        }

        for ( size_t i = 0; i < _arrayEntries.numRegexes(); i++ ) {

            bool found = false;
            for ( BSONElementSet::const_iterator j = all.begin(); j != all.end(); ++j ) {
                BSONElement bar = *j;
                if ( _arrayEntries.regex(i)->matchesSingleElement( bar ) ) {
                    found = true;
                    break;
                }
            }
            if ( ! found )
                return false;

        }

        return true;
    }

    void AllMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " $all TODO\n";
    }

    bool AllMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const AllMatchExpression* realOther = static_cast<const AllMatchExpression*>( other );
        return _path == realOther->_path && _arrayEntries.equivalent( realOther->_arrayEntries );
    }

    // -------

    bool ArrayMatchingMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {

        FieldRef path;
        path.parse(_path);

        bool traversedArray = false;
        int32_t idxPath = 0;
        BSONElement e = doc->getFieldDottedOrArray( path, &idxPath, &traversedArray );

        string rest = pathToString( path, idxPath+1 );

        if ( rest.size() == 0 ) {
            if ( e.type() == Array )
                return matchesArray( e.Obj(), details );
            return false;
        }

        if ( e.type() != Array )
            return false;

        BSONObjIterator i( e.Obj() );
        while ( i.more() ) {
            BSONElement x = i.next();
            if ( ! x.isABSONObj() )
                continue;

            BSONElement sub = x.Obj().getFieldDotted( rest );
            if ( sub.type() != Array )
                continue;

            if ( matchesArray( sub.Obj(), NULL ) ) {
                if ( details && details->needRecord() ) {
                    // trying to match crazy semantics??
                    details->setElemMatchKey( x.fieldName() );
                }
                return true;
            }
        }

        return false;
    }

    bool ArrayMatchingMatchExpression::matchesSingleElement( const BSONElement& e ) const {
        if ( e.type() != Array )
            return false;
        return matchesArray( e.Obj(), NULL );
    }


    bool ArrayMatchingMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const ArrayMatchingMatchExpression* realOther =
            static_cast<const ArrayMatchingMatchExpression*>( other );

        if ( _path != realOther->_path )
            return false;

        if ( numChildren() != realOther->numChildren() )
            return false;

        for ( unsigned i = 0; i < numChildren(); i++ )
            if ( !getChild(i)->equivalent( realOther->getChild(i) ) )
                return false;
        return true;
    }


    // -------

    Status ElemMatchObjectMatchExpression::init( const StringData& path, const MatchExpression* sub ) {
        _path = path;
        _sub.reset( sub );
        return Status::OK();
    }



    bool ElemMatchObjectMatchExpression::matchesArray( const BSONObj& anArray, MatchDetails* details ) const {
        BSONObjIterator i( anArray );
        while ( i.more() ) {
            BSONElement inner = i.next();
            if ( !inner.isABSONObj() )
                continue;
            if ( _sub->matchesBSON( inner.Obj(), NULL ) ) {
                if ( details && details->needRecord() ) {
                    details->setElemMatchKey( inner.fieldName() );
                }
                return true;
            }
        }
        return false;
    }

    void ElemMatchObjectMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " $elemMatch\n";
        _sub->debugString( debug, level + 1 );
    }


    // -------

    ElemMatchValueMatchExpression::~ElemMatchValueMatchExpression() {
        for ( unsigned i = 0; i < _subs.size(); i++ )
            delete _subs[i];
        _subs.clear();
    }

    Status ElemMatchValueMatchExpression::init( const StringData& path, const MatchExpression* sub ) {
        init( path );
        add( sub );
        return Status::OK();
    }

    Status ElemMatchValueMatchExpression::init( const StringData& path ) {
        _path = path;
        return Status::OK();
    }


    void ElemMatchValueMatchExpression::add( const MatchExpression* sub ) {
        verify( sub );
        _subs.push_back( sub );
    }

    bool ElemMatchValueMatchExpression::matchesArray( const BSONObj& anArray, MatchDetails* details ) const {
        BSONObjIterator i( anArray );
        while ( i.more() ) {
            BSONElement inner = i.next();

            if ( _arrayElementMatchesAll( inner ) ) {
                if ( details && details->needRecord() ) {
                    details->setElemMatchKey( inner.fieldName() );
                }
                return true;
            }
        }
        return false;
    }

    bool ElemMatchValueMatchExpression::_arrayElementMatchesAll( const BSONElement& e ) const {
        for ( unsigned i = 0; i < _subs.size(); i++ ) {
            if ( !_subs[i]->matchesSingleElement( e ) )
                return false;
        }
        return true;
    }

    void ElemMatchValueMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " $elemMatch\n";
        for ( unsigned i = 0; i < _subs.size(); i++ ) {
            _subs[i]->debugString( debug, level + 1 );
        }
    }


    // ------

    AllElemMatchOp::~AllElemMatchOp() {
        for ( unsigned i = 0; i < _list.size(); i++ )
            delete _list[i];
        _list.clear();
    }

    Status AllElemMatchOp::init( const StringData& path ) {
        _path = path;
        return Status::OK();
    }

    void AllElemMatchOp::add( const ArrayMatchingMatchExpression* expr ) {
        verify( expr );
        _list.push_back( expr );
    }

    bool AllElemMatchOp::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        BSONElementSet all;
        doc->getFieldsDotted( _path, all, false );

        for ( BSONElementSet::const_iterator i = all.begin(); i != all.end(); ++i ) {
            BSONElement sub = *i;
            if ( sub.type() != Array )
                continue;
            if ( _allMatch( sub.Obj() ) ) {
                return true;
            }
        }
        return false;
    }

    bool AllElemMatchOp::matchesSingleElement( const BSONElement& e ) const {
        if ( e.type() != Array )
            return false;

        return _allMatch( e.Obj() );
    }

    bool AllElemMatchOp::_allMatch( const BSONObj& anArray ) const {
        if ( _list.size() == 0 )
            return false;
        for ( unsigned i = 0; i < _list.size(); i++ ) {
            if ( !_list[i]->matchesArray( anArray, NULL ) )
                return false;
        }
        return true;
    }


    void AllElemMatchOp::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " AllElemMatchOp: " << _path << "\n";
        for ( size_t i = 0; i < _list.size(); i++ ) {
            _list[i]->debugString( debug, level + 1);
        }
    }

    bool AllElemMatchOp::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const AllElemMatchOp* realOther = static_cast<const AllElemMatchOp*>( other );
        if ( _path != realOther->_path )
            return false;

        if ( _list.size() != realOther->_list.size() )
            return false;

        for ( unsigned i = 0; i < _list.size(); i++ )
            if ( !_list[i]->equivalent( realOther->_list[i] ) )
                return false;

        return true;
    }


    // ---------

    Status SizeMatchExpression::init( const StringData& path, int size ) {
        _path = path;
        _size = size;
        return Status::OK();
    }

    bool SizeMatchExpression::matchesArray( const BSONObj& anArray, MatchDetails* details ) const {
        if ( _size < 0 )
            return false;
        return anArray.nFields() == _size;
    }

    void SizeMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " $size : " << _size << "\n";
    }

    bool SizeMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const SizeMatchExpression* realOther = static_cast<const SizeMatchExpression*>( other );
        return _path == realOther->_path && _size == realOther->_size;
    }


    // ------------------



}
