// wiredtiger_metadata.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
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
 
#include <boost/algorithm/string/predicate.hpp>

#include "mongo/db/storage/wiredtiger/wiredtiger_metadata.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_database.h"

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context.h"

#include "mongo/db/storage_options.h"
#include "mongo/util/log.h"

#include <wiredtiger.h>

namespace mongo {

    const char * WiredTigerMetaData::WT_METADATA_URI = "table:db_metadata";
    // Key is the identifier, value is name, uri, isIndex, isDeleted, metadata
    const char * WiredTigerMetaData::WT_METADATA_CONFIG =
        "key_format=q,value_format=SSbbS,leaf_page_max=4k,internal_page_max=4k";
    const uint64_t WiredTigerMetaData::INVALID_METADATA_IDENTIFIER =
                                         std::numeric_limits<uint64_t>::max();

    WiredTigerMetaData::WiredTigerMetaData( ) : _nextId( 1 ), _isInitialized( false )
    {
    }

    WiredTigerMetaData::~WiredTigerMetaData()
    {
        // Free up our in-memory structure
    }

    void WiredTigerMetaData::initialize( WiredTigerDatabase &db )
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        if ( _isInitialized )
            return;
        WiredTigerSession swrap(db);
        WT_SESSION *s(swrap.Get());

        // Open a cursor on the metadata table. The metadata cursor is special - don't go
        // through the regular WiredTigerCursor API.
        int ret = s->open_cursor(s, WT_METADATA_URI, NULL, NULL, &_metaDataCursor);
        
        if (ret == 0)
            _populate();
        else {
            // If cursor open failed, create the metadata table.
            int ret = s->create(s, WT_METADATA_URI, WT_METADATA_CONFIG);
            invariant( ret == 0 );
            ret = s->open_cursor(s, WT_METADATA_URI, NULL, NULL, &_metaDataCursor);
            invariant( ret == 0 );
            _nextId = 1;
        }
        _isInitialized = true;
    }

    std::string WiredTigerMetaData::getTableName(uint64_t identifier)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
        invariant( itr != _tables.end() );
        return ( itr->second.name );
    }

    std::string WiredTigerMetaData::getURI(uint64_t identifier)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
        invariant( itr != _tables.end() );
        return ( itr->second.uri );
    }

    BSONObj &WiredTigerMetaData::getConfig(uint64_t identifier)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
        invariant( itr != _tables.end() );
        return ( itr->second.config );
    }

    std::string WiredTigerMetaData::getURI(std::string tableName)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        WiredTigerMetaDataMap::iterator itr;
        for (itr = _tables.begin(); itr != _tables.end(); itr++) {
            if (itr->second.isDeleted)
                continue;
            if (itr->second.name == tableName)
                return itr->second.uri;
        }
        // We expect queries to always be satisfied.
        invariant( 0 );
        return std::string( "" );
    }

    uint64_t WiredTigerMetaData::getIdentifier(std::string tableName)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        return _getIdentifierLocked( tableName );
    }

    uint64_t WiredTigerMetaData::_getIdentifierLocked(std::string tableName)
    {
        WiredTigerMetaDataMap::iterator itr;
        for (itr = _tables.begin(); itr != _tables.end(); itr++) {
            if (itr->second.isDeleted)
                continue;
            if (itr->second.name == tableName)
                return itr->first;
        }
        return INVALID_METADATA_IDENTIFIER;
    }

    uint64_t WiredTigerMetaData::generateIdentifier(std::string tableName, BSONObj config)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        uint64_t old_id;

        if ( (old_id = _getIdentifierLocked(tableName)) != INVALID_METADATA_IDENTIFIER) {
            // We shouldn't ever end up with multiple tables that have matching names.
            // Check the deleted flag, and re-try a failed delete.
            //_retryTableDrop( old_id );
        }

        uint64_t id = _generateIdentifier(tableName);
        std::string uri = _generateURI(tableName, id);

        // Add the new table to the in memory map
        MetaDataEntry entry(tableName, uri, config, _isIndexName(tableName), false);
        _tables.insert( std::pair<uint64_t, MetaDataEntry>( id, entry ) );

        // Add the new table to the WiredTiger metadata table.
        _persistEntry( id, entry );

        return id;
    }

    Status WiredTigerMetaData::remove(uint64_t identifier, bool failedDrop)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        if ( failedDrop ) {
            WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
            invariant( itr != _tables.end() );
            MetaDataEntry &entry = itr->second;
            LOG(1) << "Metadata remove drop failed, uri: " << entry.uri <<
                " name: " << entry.name << endl;
            entry.isDeleted = true;
            _persistEntry( identifier, entry );
        } else {
            // Remove the entry from the map and metadata table
            _tables.erase( identifier );
            _metaDataCursor->set_key( _metaDataCursor, identifier );
            _metaDataCursor->remove( _metaDataCursor );
            _metaDataCursor->reset(_metaDataCursor);
        }
        return Status::OK();
    }

    Status WiredTigerMetaData::rename(uint64_t identifier, std::string newName)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        uint64_t old_id;
        if ( (old_id = _getIdentifierLocked(newName)) != INVALID_METADATA_IDENTIFIER) {
            LOG(1) << "Metadata rename to an existing name: " << newName << old_id << endl;
            invariant ( 0 );
        }

        WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
        invariant( itr != _tables.end() );
        MetaDataEntry &entry = itr->second;
        entry.name = newName;
        _persistEntry( identifier, entry );
        return Status::OK();
    }

    Status WiredTigerMetaData::updateConfig(uint64_t identifier, BSONObj newConfig)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
        invariant( itr != _tables.end() );
        MetaDataEntry &entry = itr->second;
        entry.config = newConfig;
        _persistEntry( identifier, entry );
        return Status::OK();
    }

    std::vector<uint64_t> WiredTigerMetaData::getAllTables()
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        WiredTigerMetaDataMap::iterator itr;
        std::vector<uint64_t> tables;
        for( itr = _tables.begin(); itr != _tables.end(); ++itr ) {
            if ( !itr->second.isDeleted && !itr->second.isIndex )
                tables.push_back( itr->first );
        }
        return tables;
    }

    std::vector<uint64_t> WiredTigerMetaData::getAllIndexes(uint64_t identifier)
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
        invariant( itr != _tables.end() );
        MetaDataEntry &primary = itr->second;

        std::vector<uint64_t> indexes;
        for( itr = _tables.begin(); itr != _tables.end(); ++itr ) {
            if ( !itr->second.isDeleted && itr->second.isIndex &&
                  boost::starts_with(itr->second.name, primary.name) )
                indexes.push_back( itr->first );
        }
        return indexes;
    }

    std::vector<uint64_t> WiredTigerMetaData::getDeleted()
    {
        boost::mutex::scoped_lock lk( _metaDataLock );
        WiredTigerMetaDataMap::iterator itr;
        std::vector<uint64_t> tables;
        for( itr = _tables.begin(); itr != _tables.end(); ++itr ) {
            if ( itr->second.isDeleted )
                tables.push_back( itr->first );
        }
        return tables;
    }

    // Internal methods.
    Status WiredTigerMetaData::_populate()
    {
        uint64_t identifier, max_id;
        const char *cTableName, *cURI;
        bool isIndex;
        const char *config;
        max_id = 0;
        while ( _metaDataCursor->next(_metaDataCursor) == 0 )
        {
            _metaDataCursor->get_key(_metaDataCursor, &identifier);
            _metaDataCursor->get_value(_metaDataCursor, &cTableName, &cURI, &isIndex, &config);
            BSONObj b( fromjson(std::string(config)));

            std::string tableName(cTableName);
            std::string uri(cURI);
            _tables.insert( std::pair<uint64_t, MetaDataEntry>( identifier, 
                MetaDataEntry(tableName, uri, b, isIndex, false) ) );
            if (identifier > max_id)
                max_id = identifier;
        }

        _metaDataCursor->reset(_metaDataCursor);

        _nextId = max_id + 1;

        return Status::OK();
    }

    uint64_t WiredTigerMetaData::_generateIdentifier(std::string tableName)
    {
        return ( ++_nextId );
        //return ( _nextId.fetchAndAdd(1) );
    }

    bool WiredTigerMetaData::_isIndexName(std::string tableName)
    {
        return ( tableName.find( ".$" ) != string::npos );
    }

    std::string WiredTigerMetaData::_generateURI(std::string tableName, uint64_t id)
    {
        std::ostringstream uri;
        
        uri << "table:" << tableName << "_" << id;
        return uri.str();
    }

    void WiredTigerMetaData::_persistEntry(uint64_t id, MetaDataEntry &entry)
    {
        _metaDataCursor->set_key(_metaDataCursor, id);
        _metaDataCursor->set_value(_metaDataCursor,
                     entry.name.c_str(),
                     entry.uri.c_str(),
                     entry.isIndex,
                     entry.isDeleted,
                     entry.config.jsonString().c_str());
        int ret = _metaDataCursor->insert(_metaDataCursor);
        invariant( ret == 0 );
        _metaDataCursor->reset(_metaDataCursor);
    }
}
