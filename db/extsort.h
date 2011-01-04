// extsort.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

#include "../pch.h"
#include "jsobj.h"
#include "namespace-inl.h"
#include "curop-inl.h"
#include "../util/array.h"

namespace mongo {


    /**
       for sorting by BSONObj and attaching a value
     */
    class BSONObjExternalSorter : boost::noncopyable {
    public:

        typedef pair<BSONObj,DiskLoc> Data;

    private:
        static BSONObj extSortOrder;

        static int extSortComp( const void *lv, const void *rv ) {
            RARELY killCurrentOp.checkForInterrupt();
            _compares++;
            Data * l = (Data*)lv;
            Data * r = (Data*)rv;
            int cmp = l->first.woCompare( r->first , extSortOrder );
            if ( cmp )
                return cmp;
            return l->second.compare( r->second );
        };

        class FileIterator : boost::noncopyable {
        public:
            FileIterator( string file );
            ~FileIterator();
            bool more();
            Data next();
        private:
            MemoryMappedFile _file;
            char * _buf;
            char * _end;
        };

        class MyCmp {
        public:
            MyCmp( const BSONObj & order = BSONObj() ) : _order( order ) {}
            bool operator()( const Data &l, const Data &r ) const {
                RARELY killCurrentOp.checkForInterrupt();
                _compares++;
                int x = l.first.woCompare( r.first , _order );
                if ( x )
                    return x < 0;
                return l.second.compare( r.second ) < 0;
            };

        private:
            BSONObj _order;
        };

    public:

        typedef FastArray<Data> InMemory;

        class Iterator : boost::noncopyable {
        public:

            Iterator( BSONObjExternalSorter * sorter );
            ~Iterator();
            bool more();
            Data next();

        private:
            MyCmp _cmp;
            vector<FileIterator*> _files;
            vector< pair<Data,bool> > _stash;

            InMemory * _in;
            InMemory::iterator _it;

        };

        BSONObjExternalSorter( const BSONObj & order = BSONObj() , long maxFileSize = 1024 * 1024 * 100 );
        ~BSONObjExternalSorter();

        void add( const BSONObj& o , const DiskLoc & loc );
        void add( const BSONObj& o , int a , int b ) {
            add( o , DiskLoc( a , b ) );
        }

        /* call after adding values, and before fetching the iterator */
        void sort();

        auto_ptr<Iterator> iterator() {
            uassert( 10052 ,  "not sorted" , _sorted );
            return auto_ptr<Iterator>( new Iterator( this ) );
        }

        int numFiles() {
            return _files.size();
        }

        long getCurSizeSoFar() { return _curSizeSoFar; }

        void hintNumObjects( long long numObjects ) {
            if ( numObjects < _arraySize )
                _arraySize = (int)(numObjects + 100);
        }

    private:

        void _sortInMem();

        void sort( string file );
        void finishMap();

        BSONObj _order;
        long _maxFilesize;
        path _root;

        int _arraySize;
        InMemory * _cur;
        long _curSizeSoFar;

        list<string> _files;
        bool _sorted;

        static unsigned long long _compares;
    };
}
