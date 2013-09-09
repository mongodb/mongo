// path.cpp

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

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/path_internal.h"
#include "mongo/db/matcher/path.h"

namespace mongo {

    Status ElementPath::init( const StringData& path ) {
        _shouldTraverseLeafArray = true;
        _fieldRef.parse( path );
        return Status::OK();
    }

    // -----

    ElementIterator::~ElementIterator(){
    }

    void ElementIterator::Context::reset() {
        _element = BSONElement();
    }

    void ElementIterator::Context::reset( BSONElement element,
                                          BSONElement arrayOffset,
                                          bool outerArray ) {
        _element = element;
        _arrayOffset = arrayOffset;
        _outerArray = outerArray;
    }


    // ------

    SimpleArrayElementIterator::SimpleArrayElementIterator( const BSONElement& theArray, bool returnArrayLast )
        : _theArray( theArray ), _returnArrayLast( returnArrayLast ), _iterator( theArray.Obj() ) {

    }

    bool SimpleArrayElementIterator::more() {
        return _iterator.more() || _returnArrayLast;
    }

    ElementIterator::Context SimpleArrayElementIterator::next() {
        if ( _iterator.more() ) {
            Context e;
            e.reset( _iterator.next(), BSONElement(), false );
            return e;
        }
        _returnArrayLast = false;
        Context e;
        e.reset( _theArray, BSONElement(), true );
        return e;
    }



    // ------
    BSONElementIterator::BSONElementIterator() {
        _path = NULL;
    }

    BSONElementIterator::BSONElementIterator( const ElementPath* path, const BSONObj& context )
        : _path( path ), _context( context ) {
        _state = BEGIN;
        //log() << "path: " << path.fieldRef().dottedField() << " context: " << context << endl;
    }

    BSONElementIterator::~BSONElementIterator() {
    }

    void BSONElementIterator::reset( const ElementPath* path, const BSONObj& context ) {
        _path = path;
        _context = context;
        _state = BEGIN;
        _next.reset();

        _subCursor.reset();
        _subCursorPath.reset();
    }


    void BSONElementIterator::ArrayIterationState::reset( const FieldRef& ref, int start ) {
        restOfPath = ref.dottedField( start );
        hasMore = restOfPath.size() > 0;
        if ( hasMore ) {
            nextPieceOfPath = ref.getPart( start );
            nextPieceOfPathIsNumber = isAllDigits( nextPieceOfPath );
        }
        else {
            nextPieceOfPathIsNumber = false;
        }
    }

    bool BSONElementIterator::ArrayIterationState::isArrayOffsetMatch( const StringData& fieldName ) const {
        if ( !nextPieceOfPathIsNumber )
            return false;
        return nextPieceOfPath == fieldName;
    }


    void BSONElementIterator::ArrayIterationState::startIterator( BSONElement e ) {
        _theArray = e;
        _iterator.reset( new BSONObjIterator( _theArray.Obj() ) );
    }

    bool BSONElementIterator::ArrayIterationState::more() {
        return _iterator && _iterator->more();
    }

    BSONElement BSONElementIterator::ArrayIterationState::next() {
        _current = _iterator->next();
        return _current;
    }


    bool BSONElementIterator::more() {
        if ( _subCursor ) {

            if ( _subCursor->more() )
                return true;

            _subCursor.reset();

            if ( _arrayIterationState.isArrayOffsetMatch( _arrayIterationState._current.fieldName() ) ) {
                if ( _arrayIterationState.nextEntireRest() ) {
                    _next.reset( _arrayIterationState._current, _arrayIterationState._current, true );
                    _arrayIterationState._current = BSONElement();
                    return true;
                }

                _subCursorPath.reset( new ElementPath() );
                _subCursorPath->init( _arrayIterationState.restOfPath.substr( _arrayIterationState.nextPieceOfPath.size() + 1 ) );
                _subCursorPath->setTraverseLeafArray( _path->shouldTraverseLeafArray() );
                _subCursor.reset( new BSONElementIterator( _subCursorPath.get(), _arrayIterationState._current.Obj() ) );
                _arrayIterationState._current = BSONElement();
                return more();
            }

        }

        if ( !_next.element().eoo() )
            return true;

        if ( _state == DONE ){
            return false;
        }

        if ( _state == BEGIN ) {
            size_t idxPath = 0;
            BSONElement e = getFieldDottedOrArray( _context, _path->fieldRef(), &idxPath );

            if ( e.type() != Array ) {
                _next.reset( e, BSONElement(), false );
                _state = DONE;
                return true;
            }

            // its an array

            _arrayIterationState.reset( _path->fieldRef(), idxPath + 1 );

            if ( !_arrayIterationState.hasMore && !_path->shouldTraverseLeafArray() ) {
                _next.reset( e, BSONElement(), true );
                _state = DONE;
                return true;
            }

            _arrayIterationState.startIterator( e );
            _state = IN_ARRAY;
            return more();
        }

        if ( _state == IN_ARRAY ) {

            while ( _arrayIterationState.more() ) {

                BSONElement x = _arrayIterationState.next();
                if ( !_arrayIterationState.hasMore ) {
                    _next.reset( x, x, false );
                    return true;
                }

                // i have deeper to go

                if ( x.type() == Object ) {
                    _subCursorPath.reset( new ElementPath() );
                    _subCursorPath->init( _arrayIterationState.restOfPath );
                    _subCursorPath->setTraverseLeafArray( _path->shouldTraverseLeafArray() );

                    _subCursor.reset( new BSONElementIterator( _subCursorPath.get(), x.Obj() ) );
                    return more();
                }


                if ( _arrayIterationState.isArrayOffsetMatch( x.fieldName() ) ) {

                    if ( _arrayIterationState.nextEntireRest() ) {
                        _next.reset( x, x, false );
                        return true;
                    }

                    if ( x.isABSONObj() ) {
                        _subCursorPath.reset( new ElementPath() );
                        _subCursorPath->init( _arrayIterationState.restOfPath.substr( _arrayIterationState.nextPieceOfPath.size() + 1 ) );
                        _subCursorPath->setTraverseLeafArray( _path->shouldTraverseLeafArray() );
                        BSONElementIterator* real = new BSONElementIterator( _subCursorPath.get(), _arrayIterationState._current.Obj() );
                        _subCursor.reset( real );
                        real->_arrayIterationState.reset( _subCursorPath->fieldRef(), 0 );
                        real->_arrayIterationState.startIterator( x );
                        real->_state = IN_ARRAY;
                        _arrayIterationState._current = BSONElement();
                        return more();
                    }
                }

            }

            if ( _arrayIterationState.hasMore )
                return false;

            _next.reset( _arrayIterationState._theArray, BSONElement(), true );
            _state = DONE;
            return true;
        }

        return false;
    }

    ElementIterator::Context BSONElementIterator::next() {
        if ( _subCursor ) {
            Context e = _subCursor->next();
            e.setArrayOffset( _arrayIterationState._current );
            return e;
        }
        Context x = _next;
        _next.reset();
        return x;
    }


}
