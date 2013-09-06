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

#include "mongo/pch.h"

#include "mongo/db/index.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/curop-inl.h"
#include "mongo/util/array.h"

#define MONGO_USE_NEW_SORTER 1

#if MONGO_USE_NEW_SORTER
#   include "mongo/db/sorter/sorter.h"
#endif

namespace mongo {

    typedef pair<BSONObj, DiskLoc> ExternalSortDatum;

    /**
     * To external sort, you provide a pointer to an implementation of this class.
     * The compare function follows the usual -1, 0, 1 semantics.
     */
    class ExternalSortComparison {
    public:
        virtual ~ExternalSortComparison() { }
        virtual int compare(const ExternalSortDatum& l, const ExternalSortDatum& r) const = 0;
    };

#if MONGO_USE_NEW_SORTER
    // TODO This class will probably disappear in the future or be replaced with a typedef
    class BSONObjExternalSorter : boost::noncopyable {
    public:
        typedef pair<BSONObj, DiskLoc> Data;
        typedef SortIteratorInterface<BSONObj, DiskLoc> Iterator;

        BSONObjExternalSorter(const ExternalSortComparison* comp, long maxFileSize=100*1024*1024);

        void add( const BSONObj& o, const DiskLoc& loc, bool mayInterrupt ) {
            *_mayInterrupt = mayInterrupt;
            _sorter->add(o.getOwned(), loc);
        }

        auto_ptr<Iterator> iterator() { return auto_ptr<Iterator>(_sorter->done()); }

        void sort( bool mayInterrupt ) { *_mayInterrupt = mayInterrupt; }
        int numFiles() { return _sorter->numFiles(); }
        long getCurSizeSoFar() { return _sorter->memUsed(); }
        void hintNumObjects(long long) {} // unused

    private:
        shared_ptr<bool> _mayInterrupt;
        scoped_ptr<Sorter<BSONObj, DiskLoc> > _sorter;
    };
#else
    /**
       for external (disk) sorting by BSONObj and attaching a value
     */
    class BSONObjExternalSorter : boost::noncopyable {
    public:
        BSONObjExternalSorter(const ExternalSortComparison* cmp,
                              long maxFileSize = 1024 * 1024 * 100 );
        ~BSONObjExternalSorter();
 
    private:
        static HLMutex _extSortMutex;

        static int _compare(const ExternalSortComparison* cmp, const ExternalSortDatum& l,
                            const ExternalSortDatum& r);

        class MyCmp {
        public:
            MyCmp(const ExternalSortComparison* cmp) : _cmp(cmp) { }
            bool operator()( const ExternalSortDatum &l, const ExternalSortDatum &r ) const {
                return _cmp->compare(l, r) < 0;
            };
        private:
            const ExternalSortComparison* _cmp;
        };

        static bool extSortMayInterrupt;
        static int extSortComp( const void *lv, const void *rv );
        static const ExternalSortComparison* staticExtSortCmp;

        class FileIterator : boost::noncopyable {
        public:
            FileIterator( const std::string& file );
            ~FileIterator();
            bool more();
            ExternalSortDatum next();
        private:
            bool _read( char* buf, long long count );

            int _file;
            unsigned long long _length;
            unsigned long long _readSoFar;
        };

    public:

        typedef FastArray<ExternalSortDatum> InMemory;

        class Iterator : boost::noncopyable {
        public:

            Iterator( BSONObjExternalSorter * sorter );
            ~Iterator();
            bool more();
            ExternalSortDatum next();

        private:
            MyCmp _cmp;
            vector<FileIterator*> _files;
            vector< pair<ExternalSortDatum,bool> > _stash;

            InMemory * _in;
            InMemory::iterator _it;

        };

        void add( const BSONObj& o, const DiskLoc& loc, bool mayInterrupt );

        /* call after adding values, and before fetching the iterator */
        void sort( bool mayInterrupt );

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

        void _sortInMem( bool mayInterrupt );

        void sort( const std::string& file );
        void finishMap( bool mayInterrupt );

        const ExternalSortComparison* _cmp;
        long _maxFilesize;
        boost::filesystem::path _root;

        int _arraySize;
        InMemory * _cur;
        long _curSizeSoFar;

        list<string> _files;
        bool _sorted;

        static unsigned long long _compares;
        static unsigned long long _uniqueNumber;
    };
#endif
}
