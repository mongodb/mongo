// wiredtiger_size_storer.cpp

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
        Entry& entry = _entries[rs->GetURI()];
        entry.rs = rs;
        entry.numRecords = numRecords;
        entry.dataSize = dataSize;
        entry.dirty = true;
    }

    void WiredTigerSizeStorer::onDestroy( WiredTigerRecordStore* rs ) {
        _checkMagic();
        boost::mutex::scoped_lock lk( _entriesMutex );
        Entry& entry = _entries[rs->GetURI()];
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
                string uri( reinterpret_cast<const char*>( key.data ), key.size );
                BSONObj data( reinterpret_cast<const char*>( value.data ) );

                LOG(2) << "WiredTigerSizeStorer::loadFrom " << uri << " -> " << data;

                Entry& e = m[uri];
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
                string uri = it->first;
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
                myMap[uri] = entry;
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
            string uri = it->first;
            Entry& entry = it->second;

            BSONObj data;
            {
                BSONObjBuilder b;
                b.append( "numRecords", entry.numRecords );
                b.append( "dataSize", entry.dataSize );
                data = b.obj();
            }

            LOG(2) << "WiredTigerSizeStorer::storeInto " << uri << " -> " << data;

            WiredTigerItem key( uri.c_str(), uri.size() );
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
