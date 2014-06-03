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

#include "mongo/db/matcher/expression_array.h"

#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"

namespace mongo {

    Status ArrayMatchingMatchExpression::initPath( const StringData& path ) {
        _path = path;
        Status s = _elementPath.init( _path );
        _elementPath.setTraverseLeafArray( false );
        return s;
    }

    bool ArrayMatchingMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        MatchableDocument::IteratorHolder cursor( doc, &_elementPath );

        while ( cursor->more() ) {
            ElementIterator::Context e = cursor->next();
            if ( e.element().type() != Array )
                continue;

            bool amIRoot = e.arrayOffset().eoo();

            if ( !matchesArray( e.element().Obj(), amIRoot ? details : NULL ) )
                continue;

            if ( !amIRoot && details && details->needRecord() && !e.arrayOffset().eoo() ) {
                details->setElemMatchKey( e.arrayOffset().fieldName() );
            }
            return true;
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

    Status ElemMatchObjectMatchExpression::init( const StringData& path, MatchExpression* sub ) {
        _sub.reset( sub );
        return initPath( path );
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
        debug << path() << " $elemMatch (obj)";

        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
        _sub->debugString( debug, level + 1 );
    }

    void ElemMatchObjectMatchExpression::toBSON(BSONObjBuilder* out) const {
        BSONObjBuilder subBob;
        _sub->toBSON(&subBob);
        if (path().empty()) {
            out->append("$elemMatch", subBob.obj());
        }
        else {
            out->append(path(), BSON("$elemMatch" << subBob.obj()));
        }
    }


    // -------

    ElemMatchValueMatchExpression::~ElemMatchValueMatchExpression() {
        for ( unsigned i = 0; i < _subs.size(); i++ )
            delete _subs[i];
        _subs.clear();
    }

    Status ElemMatchValueMatchExpression::init( const StringData& path, MatchExpression* sub ) {
        init( path );
        add( sub );
        return Status::OK();
    }

    Status ElemMatchValueMatchExpression::init( const StringData& path ) {
        return initPath( path );
    }


    void ElemMatchValueMatchExpression::add( MatchExpression* sub ) {
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
        debug << path() << " $elemMatch (value)";

        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
        for ( unsigned i = 0; i < _subs.size(); i++ ) {
            _subs[i]->debugString( debug, level + 1 );
        }
    }

    void ElemMatchValueMatchExpression::toBSON(BSONObjBuilder* out) const {
        BSONObjBuilder emBob;
        for ( unsigned i = 0; i < _subs.size(); i++ ) {
            _subs[i]->toBSON(&emBob);
        }
        if (path().empty()) {
            out->append("$elemMatch", emBob.obj());
        }
        else {
            out->append(path(), BSON("$elemMatch" << emBob.obj()));
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
        Status s = _elementPath.init( _path );
        _elementPath.setTraverseLeafArray( false );
        return s;
    }

    void AllElemMatchOp::add( ArrayMatchingMatchExpression* expr ) {
        verify( expr );
        _list.push_back( expr );
    }

    bool AllElemMatchOp::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        MatchableDocument::IteratorHolder cursor( doc, &_elementPath );
        while ( cursor->more() ) {
            ElementIterator::Context e = cursor->next();
            if ( e.element().type() != Array )
                continue;
            if ( _allMatch( e.element().Obj() ) )
                return true;
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
            if ( !static_cast<ArrayMatchingMatchExpression*>(_list[i])->matchesArray(
                     anArray, NULL ) )
                return false;
        }
        return true;
    }


    void AllElemMatchOp::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << _path << " AllElemMatchOp:";
        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
        for ( size_t i = 0; i < _list.size(); i++ ) {
            _list[i]->debugString( debug, level + 1);
        }
    }

    void AllElemMatchOp::toBSON(BSONObjBuilder* out) const {
        BSONObjBuilder allBob(out->subobjStart(path()));

        BSONArrayBuilder arrBob(allBob.subarrayStart("$all"));
        for (unsigned i = 0; i < _list.size(); i++) {
            BSONObjBuilder childBob(arrBob.subobjStart());
            _list[i]->toBSON(&childBob);
        }
        arrBob.doneFast();

        allBob.doneFast();
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
        _size = size;
        return initPath( path );
    }

    bool SizeMatchExpression::matchesArray( const BSONObj& anArray, MatchDetails* details ) const {
        if ( _size < 0 )
            return false;
        return anArray.nFields() == _size;
    }

    void SizeMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << path() << " $size : " << _size << "\n";

        MatchExpression::TagData* td = getTag();
        if (NULL != td) {
            debug << " ";
            td->debugString(&debug);
        }
    }

    void SizeMatchExpression::toBSON(BSONObjBuilder* out) const {
        out->append(path(), BSON("$size" << _size));
    }

    bool SizeMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;

        const SizeMatchExpression* realOther = static_cast<const SizeMatchExpression*>( other );
        return path() == realOther->path() && _size == realOther->_size;
    }


    // ------------------



}
