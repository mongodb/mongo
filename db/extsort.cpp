// extsort.cpp

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

#include "../stdafx.h"

#include "extsort.h"
#include "namespace.h"
#include "../util/file.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace mongo {

    BSONObjExternalSorter::BSONObjExternalSorter( const BSONObj & order , long maxFileSize )
        : _order( order ) , _maxFilesize( maxFileSize ) , 
          _map(0), _mapSizeSoFar(0), _largestObject(0),  _sorted(0){
        
        stringstream rootpath;
        rootpath << dbpath;
	if ( dbpath[dbpath.size()-1] != '/' )
	  rootpath << "/";
	rootpath << "esort." << time(0) << "." << rand() << "/";
        _root = rootpath.str();
        
        create_directories( _root );

    }
    
    BSONObjExternalSorter::~BSONObjExternalSorter(){
        if ( _map ){
            delete _map;
            _map = 0;
        }
        
        remove_all( _root );
    }

    void BSONObjExternalSorter::sort(){
        uassert( "already sorted" , ! _sorted );

        _sorted = true;

        if ( _map ){
            finishMap();
        }
        
        if ( _map ){
            delete _map;
            _map = 0;
        }
        
        if ( _files.size() == 0 )
            return;
        
    }

    void BSONObjExternalSorter::add( const BSONObj& o , const DiskLoc & loc ){
        uassert( "sorted already" , ! _sorted );
        
        if ( ! _map ){
            _map = new multimap<BSONObj,DiskLoc,BSONObjCmp>( _order );
        }
        
        _map->insert( pair<BSONObj,DiskLoc>( o , loc ) );

        long size = o.objsize();
        _mapSizeSoFar += size + sizeof( DiskLoc );
        if ( size > _largestObject )
            _largestObject = size;
        
        if ( _mapSizeSoFar > _maxFilesize )
            finishMap();

    }
    
    void BSONObjExternalSorter::finishMap(){
        uassert( "bad" , _map );
        
        _mapSizeSoFar = 0;
        if ( _map->size() == 0 )
            return;
        
        stringstream ss;
        ss << _root.string() << "file." << _files.size();
        string file = ss.str();
        
        int out = open( file.c_str() , O_WRONLY | O_CREAT | O_TRUNC , 0666 );
        uassert( (string)"couldn't open file: " + file , out > 0 );
        
        int num = 0;
        for ( multimap<BSONObj,DiskLoc,BSONObjCmp>::iterator i=_map->begin(); i != _map->end(); i++ ){
            pair<BSONObj,DiskLoc> p = *i;
            assert( write( out , p.first.objdata() , p.first.objsize() ) > 0 );
            assert( write( out , & p.second , sizeof( DiskLoc ) ) > 0 );
            num++;
        }
        
        _map->clear();
        
        _files.push_back( file );
        close( out );

        log(2) << "Added file: " << file << " with " << num << "objects for external sort" << endl;
    }
    
    // ---------------------------------

    BSONObjExternalSorter::Iterator::Iterator( BSONObjExternalSorter * sorter ) : _cmp( sorter->_order ){
        for ( list<string>::iterator i=sorter->_files.begin(); i!=sorter->_files.end(); i++ ){
            _files.push_back( new FileIterator( *i , sorter->_largestObject + 256 ) );
            _stash.push_back( pair<Data,bool>( Data( BSONObj() , DiskLoc() ) , false ) );
        }
    }
    
    BSONObjExternalSorter::Iterator::~Iterator(){
        for ( vector<FileIterator*>::iterator i=_files.begin(); i!=_files.end(); i++ )
            delete *i;
        _files.clear();
    }
    
    bool BSONObjExternalSorter::Iterator::more(){
        for ( vector<FileIterator*>::iterator i=_files.begin(); i!=_files.end(); i++ )
            if ( (*i)->more() )
                return true;
        for ( vector< pair<Data,bool> >::iterator i=_stash.begin(); i!=_stash.end(); i++ )
            if ( i->second )
                return true;
        return false;
    }
        
    pair<BSONObj,DiskLoc> BSONObjExternalSorter::Iterator::next(){
        Data best;
        int slot = -1;
        
        for ( unsigned i=0; i<_stash.size(); i++ ){

            if ( ! _stash[i].second ){
                if ( _files[i]->more() )
                    _stash[i] = pair<Data,bool>( _files[i]->next() , true );
                else
                    continue;
            }
            
            if ( slot == -1 || _cmp( best.first , _stash[i].first.first ) == 0 ){
                best = _stash[i].first;
                slot = i;
            }
                
        }
        
        assert( slot >= 0 );
        _stash[slot].second = false;

        return best;
    }

    // -----------------------------------
    
    BSONObjExternalSorter::FileIterator::FileIterator( string file , int bufSize ) : _fd(0),_buf(0){
        _fd = open( file.c_str() , O_RDONLY );
        uassert( (string)"couldn't open file:" + file , _fd > 0 );

        _buf = (char*)malloc( bufSize );
        assert( _buf );

        _length = file_size( file );
        _read = 0;
    }
    BSONObjExternalSorter::FileIterator::~FileIterator(){
        if ( _fd ){
            close( _fd );
            _fd = 0;
        }
        
        if ( _buf ){
            free( _buf );
            _buf = 0;
        }
    }
    
    bool BSONObjExternalSorter::FileIterator::more(){
        return _read < _length;
    }
    
    pair<BSONObj,DiskLoc> BSONObjExternalSorter::FileIterator::next(){
        assert( 4 == read( _fd , _buf , 4 ) );
        int toread = ((int*)_buf)[0] - 4;
        assert( toread == read( _fd , _buf + 4 , toread ) );
        
        BSONObj o( _buf );
        DiskLoc l;
        assert( sizeof( DiskLoc ) == read( _fd , &l , sizeof( DiskLoc ) ) );
        
        _read += 4 + toread + sizeof( DiskLoc );

        return Data( o , l );
    }
    
}
