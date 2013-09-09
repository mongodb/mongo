// path.h

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

#pragma once

#include <boost/scoped_ptr.hpp>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjiterator.h"
#include "mongo/db/field_ref.h"

namespace mongo {

    class ElementPath {
    public:
        Status init( const StringData& path );

        void setTraverseLeafArray( bool b ) { _shouldTraverseLeafArray = b; }

        const FieldRef& fieldRef() const { return _fieldRef; }
        bool shouldTraverseLeafArray() const { return _shouldTraverseLeafArray; }

    private:
        FieldRef _fieldRef;
        bool _shouldTraverseLeafArray;
    };

    class ElementIterator {
    public:
        class Context {
        public:

            void reset();

            void reset( BSONElement element, BSONElement arrayOffset, bool outerArray );

            void setArrayOffset( BSONElement e ) { _arrayOffset = e; }

            BSONElement element() const { return _element; }
            BSONElement arrayOffset() const { return _arrayOffset; }
            bool outerArray() const { return _outerArray; }

        private:
            BSONElement _element;
            BSONElement _arrayOffset;
            bool _outerArray;
        };

        virtual ~ElementIterator();

        virtual bool more() = 0;
        virtual Context next() = 0;

    };

    // ---------------------------------------------------------------

    class SingleElementElementIterator : public ElementIterator {
    public:
        explicit SingleElementElementIterator( BSONElement e )
            : _seen( false ) {
            _element.reset( e, BSONElement(), false );
        }
        virtual ~SingleElementElementIterator(){}

        virtual bool more() { return !_seen; }
        virtual Context next() { _seen = true; return _element; }

    private:
        bool _seen;
        ElementIterator::Context _element;
    };

    class SimpleArrayElementIterator : public ElementIterator {
    public:
        SimpleArrayElementIterator( const BSONElement& theArray, bool returnArrayLast );

        virtual bool more();
        virtual Context next();

    private:
        BSONElement _theArray;
        bool _returnArrayLast;
        BSONObjIterator _iterator;
    };

    class BSONElementIterator : public ElementIterator {
    public:
        BSONElementIterator();
        BSONElementIterator( const ElementPath* path, const BSONObj& context );

        virtual ~BSONElementIterator();

        void reset( const ElementPath* path, const BSONObj& context );

        bool more();
        Context next();

    private:
        const ElementPath* _path;
        BSONObj _context;

        enum State { BEGIN, IN_ARRAY, DONE } _state;
        Context _next;

        struct ArrayIterationState {

            void reset( const FieldRef& ref, int start );
            void startIterator( BSONElement theArray );

            bool more();
            BSONElement next();

            bool isArrayOffsetMatch( const StringData& fieldName ) const;
            bool nextEntireRest() const { return nextPieceOfPath.size() == restOfPath.size(); }

            string restOfPath;
            bool hasMore;
            StringData nextPieceOfPath;
            bool nextPieceOfPathIsNumber;

            BSONElement _theArray;
            BSONElement _current;
            boost::scoped_ptr<BSONObjIterator> _iterator;
        };

        ArrayIterationState _arrayIterationState;

        boost::scoped_ptr<ElementIterator> _subCursor;
        boost::scoped_ptr<ElementPath> _subCursorPath;
    };

}
