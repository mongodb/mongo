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

#include "../stdafx.h"

#include "jsobj.h"
#include "namespace.h"

#include <map>

namespace mongo {
    
    /**
       for sorting by BSONObj and attaching a value
     */
    class BSONObjExternalSorter : boost::noncopyable {
    public:
        
        typedef pair<BSONObj,DiskLoc> Data;

        class FileIterator : boost::noncopyable {
        public:
            FileIterator( string file , int bufSize );
            ~FileIterator();
            bool more();
            Data next();            
        private:
            int _fd;
            char * _buf;
            long _read;
            long _length;
        };
        
        class Iterator {
        public:
            
            Iterator( BSONObjExternalSorter * sorter );
            ~Iterator();
            bool more();
            Data next();
            
        private:
            BSONObjCmp _cmp;
            vector<FileIterator*> _files;
            vector< pair<Data,bool> > _stash;
        };
        
        BSONObjExternalSorter( const BSONObj & order = BSONObj() , long maxFileSize = 1024 * 1024 * 100 );
        ~BSONObjExternalSorter();
        
        void add( const BSONObj& o , const DiskLoc & loc );
        void add( const BSONObj& o , int a , int b ){
            add( o , DiskLoc( a , b ) );
        }

        void sort();
        
        Iterator iterator(){
            uassert( "not sorted" , _sorted );
            return Iterator( this );
        }
        
    private:
        
        void sort( string file );
        void finishMap();
        
        BSONObj _order;
        long _maxFilesize;
        path _root;
        
        multimap<BSONObj,DiskLoc,BSONObjCmp> * _map;
        long _mapSizeSoFar;
        
        long _largestObject;

        list<string> _files;
        bool _sorted;
    };
}
