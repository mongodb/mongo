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

#include "pch.h"

#if defined(_WIN32)
#   include <io.h>
#endif

#include "extsort.h"
#include "namespace-inl.h"
#include "../util/file.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fstream>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>



namespace mongo {

    HLMutex BSONObjExternalSorter::_extSortMutex("s");
    IndexInterface *BSONObjExternalSorter::extSortIdxInterface;
    Ordering BSONObjExternalSorter::extSortOrder( Ordering::make(BSONObj()) );
    unsigned long long BSONObjExternalSorter::_compares = 0;
    unsigned long long BSONObjExternalSorter::_uniqueNumber = 0;
    static SimpleMutex _uniqueNumberMutex( "uniqueNumberMutex" );

    /*static*/
    int BSONObjExternalSorter::_compare(IndexInterface& i, const Data& l, const Data& r, const Ordering& order) { 
        RARELY killCurrentOp.checkForInterrupt();
        _compares++;
        int x = i.keyCompare(l.first, r.first, order);
        if ( x )
            return x;
        return l.second.compare( r.second );
    }

    /*static*/
    int BSONObjExternalSorter::extSortComp( const void *lv, const void *rv ) {
        DEV RARELY {
            _extSortMutex.dassertLocked(); // must be as we use a global var
        }
        Data * l = (Data*)lv;
        Data * r = (Data*)rv;
        return _compare(*extSortIdxInterface, *l, *r, extSortOrder);
    };

    BSONObjExternalSorter::BSONObjExternalSorter( IndexInterface &i, const BSONObj & order , long maxFileSize )
        : _idxi(i), _order( order.getOwned() ) , _maxFilesize( maxFileSize ) ,
          _arraySize(1000000), _cur(0), _curSizeSoFar(0), _sorted(0) {

        stringstream rootpath;
        rootpath << dbpath;
        if ( dbpath[dbpath.size()-1] != '/' )
            rootpath << "/";

        unsigned long long thisUniqueNumber;
        {
            SimpleMutex::scoped_lock lk(_uniqueNumberMutex);
            thisUniqueNumber = _uniqueNumber;
            ++_uniqueNumber;
        }
        rootpath << "_tmp/esort." << time(0) << "." << thisUniqueNumber << "/";
        _root = rootpath.str();

        log(1) << "external sort root: " << _root.string() << endl;

        create_directories( _root );
        _compares = 0;
    }

    BSONObjExternalSorter::~BSONObjExternalSorter() {
        if ( _cur ) {
            delete _cur;
            _cur = 0;
        }
        unsigned long removed = remove_all( _root );
        wassert( removed == 1 + _files.size() );
    }

    void BSONObjExternalSorter::_sortInMem() {
        // extSortComp needs to use glpbals
        // qsort_r only seems available on bsd, which is what i really want to use
        HLMutex::scoped_lock lk(_extSortMutex);
        extSortIdxInterface = &_idxi;
        extSortOrder = Ordering::make(_order);
        _cur->sort( BSONObjExternalSorter::extSortComp );
    }

    void BSONObjExternalSorter::sort() {
        uassert( 10048 ,  "already sorted" , ! _sorted );

        _sorted = true;

        if ( _cur && _files.size() == 0 ) {
            _sortInMem();
            log(1) << "\t\t not using file.  size:" << _curSizeSoFar << " _compares:" << _compares << endl;
            return;
        }

        if ( _cur ) {
            finishMap();
        }

        if ( _cur ) {
            delete _cur;
            _cur = 0;
        }

        if ( _files.size() == 0 )
            return;

    }

    void BSONObjExternalSorter::add( const BSONObj& o , const DiskLoc & loc ) {
        uassert( 10049 ,  "sorted already" , ! _sorted );

        if ( ! _cur ) {
            _cur = new InMemory( _arraySize );
        }

        Data& d = _cur->getNext();
        d.first = o.getOwned();
        d.second = loc;

        long size = o.objsize();
        _curSizeSoFar += size + sizeof( DiskLoc ) + sizeof( BSONObj );

        if (  _cur->hasSpace() == false ||  _curSizeSoFar > _maxFilesize ) {
            finishMap();
            log(1) << "finishing map" << endl;
        }

    }

    void BSONObjExternalSorter::finishMap() {
        uassert( 10050 ,  "bad" , _cur );

        _curSizeSoFar = 0;
        if ( _cur->size() == 0 )
            return;

        _sortInMem();

        stringstream ss;
        ss << _root.string() << "/file." << _files.size();
        string file = ss.str();

        // todo: it may make sense to fadvise that this not be cached so that building the index doesn't 
        //       eject other things the db is using from the file system cache.  while we will soon be reading 
        //       this back, if it fit in ram, there wouldn't have been a need for an external sort in the first 
        //       place.

        ofstream out;
        out.open( file.c_str() , ios_base::out | ios_base::binary );
        assertStreamGood( 10051 ,  (string)"couldn't open file: " + file , out );

        int num = 0;
        for ( InMemory::iterator i=_cur->begin(); i != _cur->end(); ++i ) {
            Data p = *i;
            out.write( p.first.objdata() , p.first.objsize() );
            out.write( (char*)(&p.second) , sizeof( DiskLoc ) );
            num++;
        }

        _cur->clear();

        _files.push_back( file );
        out.close();

        log(2) << "Added file: " << file << " with " << num << "objects for external sort" << endl;
    }

    // ---------------------------------

    BSONObjExternalSorter::Iterator::Iterator( BSONObjExternalSorter * sorter ) :
        _cmp( sorter->_idxi, sorter->_order ) , _in( 0 ) {

        for ( list<string>::iterator i=sorter->_files.begin(); i!=sorter->_files.end(); i++ ) {
            _files.push_back( new FileIterator( *i ) );
            _stash.push_back( pair<Data,bool>( Data( BSONObj() , DiskLoc() ) , false ) );
        }

        if ( _files.size() == 0 && sorter->_cur ) {
            _in = sorter->_cur;
            _it = sorter->_cur->begin();
        }
    }

    BSONObjExternalSorter::Iterator::~Iterator() {
        for ( vector<FileIterator*>::iterator i=_files.begin(); i!=_files.end(); i++ )
            delete *i;
        _files.clear();
    }

    bool BSONObjExternalSorter::Iterator::more() {

        if ( _in )
            return _it != _in->end();

        for ( vector<FileIterator*>::iterator i=_files.begin(); i!=_files.end(); i++ )
            if ( (*i)->more() )
                return true;
        for ( vector< pair<Data,bool> >::iterator i=_stash.begin(); i!=_stash.end(); i++ )
            if ( i->second )
                return true;
        return false;
    }

    BSONObjExternalSorter::Data BSONObjExternalSorter::Iterator::next() {

        if ( _in ) {
            Data& d = *_it;
            ++_it;
            return d;
        }

        Data best;
        int slot = -1;

        for ( unsigned i=0; i<_stash.size(); i++ ) {

            if ( ! _stash[i].second ) {
                if ( _files[i]->more() )
                    _stash[i] = pair<Data,bool>( _files[i]->next() , true );
                else
                    continue;
            }

            if ( slot == -1 || _cmp( best , _stash[i].first ) == 0 ) {
                best = _stash[i].first;
                slot = i;
            }

        }

        verify( slot >= 0 );
        _stash[slot].second = false;

        return best;
    }

    // -----------------------------------

    BSONObjExternalSorter::FileIterator::FileIterator( string file ) {
#ifdef _WIN32
        _file = ::_open( file.c_str(), _O_BINARY | _O_RDWR | _O_CREAT , _S_IREAD | _S_IWRITE );
#else
        _file = ::open( file.c_str(), O_CREAT | O_RDWR | O_NOATIME , S_IRUSR | S_IWUSR );
#endif
        massert( 16392, 
                 str::stream() << "FileIterator can't open file: " 
                 << file << errnoWithDescription(), 
                 _file >= 0 );

#ifdef POSIX_FADV_SEQUENTIAL
        int err = posix_fadvise(_file, 0, 0, POSIX_FADV_SEQUENTIAL );
        if ( err )
            log() << "posix_fadvise failed: " << err << endl;
#endif

        _length = (unsigned long long)boost::filesystem::file_size( file );
        _readSoFar = 0;
    }
    BSONObjExternalSorter::FileIterator::~FileIterator() {
        if ( _file >= 0 ) {
#ifdef _WIN32
            _close( _file );
#else
            ::close( _file );
#endif
        }
    }

    bool BSONObjExternalSorter::FileIterator::more() {
        return _readSoFar < _length;
    }


    bool BSONObjExternalSorter::FileIterator::_read( char* buf, long long count ) {
        long long total = 0;
        while ( total < count ) {
#ifdef _WIN32
            long long now = ::_read( _file, buf, count );
#else
            long long now = ::read( _file, buf, count );
#endif
            if ( now < 0 ) {
                log() << "read failed for BSONObjExternalSorter " << errnoWithDescription() << endl;
                return false;
            }
            if ( now == 0 ) {
                return false;
            }
            total += now;
            buf += now;
        }
        return true;
    }
    
    BSONObjExternalSorter::Data BSONObjExternalSorter::FileIterator::next() {
        // read BSONObj

        int size;
        verify( _read( reinterpret_cast<char*>(&size), 4 ) );
        char* buf = reinterpret_cast<char*>( malloc( sizeof(unsigned) + size ) );
        verify( buf );

        memset( buf, 0, 4 ); // for Holder
        memcpy( buf+sizeof(unsigned), reinterpret_cast<char*>(&size), sizeof(int) ); // size of doc
        if ( ! _read( buf + sizeof(unsigned) + sizeof(int), size-sizeof(int) ) ) { // doc content
            free( buf );
            msgasserted( 16394, std::string("reading doc for external sort failed:") + errnoWithDescription() );
        }
        
        // read DiskLoc
        DiskLoc l;
        if ( ! _read( reinterpret_cast<char*>(&l), 8 ) ) {
            free( buf );
            msgasserted( 16393, std::string("reading DiskLoc for external sort failed") + errnoWithDescription() );            
        }
        _readSoFar += 8 + size;
        
        BSONObj::Holder* h = reinterpret_cast<BSONObj::Holder*>(buf);
        return Data( BSONObj(h), l );
    }

}
