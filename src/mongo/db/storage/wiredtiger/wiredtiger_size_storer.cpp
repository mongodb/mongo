// wiredtiger_size_storer.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <wiredtiger.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::string;

    namespace {
        int MAGIC = 123123;
    }

    WiredTigerSizeStorer::WiredTigerSizeStorer() {
        _magic = MAGIC;
    }

    WiredTigerSizeStorer::~WiredTigerSizeStorer() {
        _magic = 11111;
    }

    void WiredTigerSizeStorer::_checkMagic() const {
        if ( _magic == MAGIC )
            return;
        log() << "WiredTigerSizeStorer magic wrong: " << _magic;
        invariant( _magic == MAGIC );
    }

    void WiredTigerSizeStorer::onCreate( WiredTigerRecordStore* rs,
                                         long long numRecords, long long dataSize ) {
        _checkMagic();
        boost::mutex::scoped_lock lk( _entriesMutex );
        Entry& entry = _entries[rs->getURI()];
        entry.rs = rs;
        entry.numRecords = numRecords;
        entry.dataSize = dataSize;
        entry.dirty = true;
    }

    void WiredTigerSizeStorer::onDestroy( WiredTigerRecordStore* rs ) {
        _checkMagic();
        boost::mutex::scoped_lock lk( _entriesMutex );
        Entry& entry = _entries[rs->getURI()];
        entry.numRecords = rs->numRecords( NULL );
        entry.dataSize = rs->dataSize( NULL );
        entry.dirty = true;
        entry.rs = NULL;
    }


    void WiredTigerSizeStorer::store( const StringData& uri,
                                      long long numRecords, long long dataSize ) {
        _checkMagic();
        boost::mutex::scoped_lock lk( _entriesMutex );
        Entry& entry = _entries[uri.toString()];
        entry.numRecords = numRecords;
        entry.dataSize = dataSize;
        entry.dirty = true;
    }

    void WiredTigerSizeStorer::load( const StringData& uri,
                                     long long* numRecords, long long* dataSize ) const {
        _checkMagic();
        boost::mutex::scoped_lock lk( _entriesMutex );
        Map::const_iterator it = _entries.find( uri.toString() );
        if ( it == _entries.end() ) {
            *numRecords = 0;
            *dataSize = 0;
            return;
        }
        *numRecords = it->second.numRecords;
        *dataSize = it->second.dataSize;
    }

    void WiredTigerSizeStorer::loadFrom( WiredTigerSession* session,
                                         const std::string& uri ) {
        _checkMagic();

        Map m;
        {
            WT_SESSION* s = session->getSession();
            WT_CURSOR* c = NULL;
            int ret = s->open_cursor( s, uri.c_str(), NULL, NULL, &c );
            if ( ret == ENOENT ) {
                // doesn't exist, we'll create later
                return;
            }
            invariantWTOK( ret );

            while ( c->next(c) == 0 ) {
                WT_ITEM key;
                WT_ITEM value;
                invariantWTOK( c->get_key(c, &key ) );
                invariantWTOK( c->get_value(c, &value ) );
                std::string uriKey( reinterpret_cast<const char*>( key.data ), key.size );
                BSONObj data( reinterpret_cast<const char*>( value.data ) );

                LOG(2) << "WiredTigerSizeStorer::loadFrom " << uriKey << " -> " << data;

                Entry& e = m[uriKey];
                e.numRecords = data["numRecords"].safeNumberLong();
                e.dataSize = data["dataSize"].safeNumberLong();
                e.dirty = false;
                e.rs = NULL;
            }
            invariantWTOK( c->close(c) );
        }

        boost::mutex::scoped_lock lk( _entriesMutex );
        _entries = m;
    }

    void WiredTigerSizeStorer::storeInto( WiredTigerSession* session,
                                          const std::string& uri ) {
        Map myMap;
        {
            boost::mutex::scoped_lock lk( _entriesMutex );
            for ( Map::iterator it = _entries.begin(); it != _entries.end(); ++it ) {
                std::string uriKey = it->first;
                Entry& entry = it->second;
                if ( entry.rs ) {
                    if ( entry.dataSize != entry.rs->dataSize( NULL ) ) {
                        entry.dataSize = entry.rs->dataSize( NULL );
                        entry.dirty = true;
                    }
                    if ( entry.numRecords != entry.rs->numRecords( NULL ) ) {
                        entry.numRecords = entry.rs->numRecords( NULL );
                        entry.dirty = true;
                    }
                }

                if ( !entry.dirty )
                    continue;
                myMap[uriKey] = entry;
            }
        }

        WT_SESSION* s = session->getSession();
        WT_CURSOR* c = NULL;
        int ret = s->open_cursor( s, uri.c_str(), NULL, NULL, &c );
        if ( ret == ENOENT ) {
            invariantWTOK( s->create( s, uri.c_str(), "" ) );
            ret = s->open_cursor( s, uri.c_str(), NULL, NULL, &c );
        }
        invariantWTOK( ret );

        for ( Map::iterator it = myMap.begin(); it != myMap.end(); ++it ) {
            string uriKey = it->first;
            Entry& entry = it->second;

            BSONObj data;
            {
                BSONObjBuilder b;
                b.append( "numRecords", entry.numRecords );
                b.append( "dataSize", entry.dataSize );
                data = b.obj();
            }

            LOG(2) << "WiredTigerSizeStorer::storeInto " << uriKey << " -> " << data;

            WiredTigerItem key( uriKey.c_str(), uriKey.size() );
            WiredTigerItem value( data.objdata(), data.objsize() );
            c->set_key( c, key.Get() );
            c->set_value( c, value.Get() );
            invariantWTOK( c->insert(c) );
            entry.dirty = false;

            c->reset(c);
        }

        invariantWTOK( c->close(c) );

    }


}
