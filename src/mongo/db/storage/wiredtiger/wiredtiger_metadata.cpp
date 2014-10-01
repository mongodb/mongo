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
 
#include "mongo/db/storage/wiredtiger/wiredtiger_metadata.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_database.h"

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context.h"

#include "mongo/db/storage_options.h"
#include "mongo/util/log.h"

#include <wiredtiger.h>

namespace mongo {

    const char * WiredTigerMetaData::WT_METADATA_URI = "table:metadata";
    // Key is the identifier, value is name, isIndex, metadata
    const char * WiredTigerMetaData::WT_METADATA_CONFIG =
        "key_format=q,value_format=SbS,leaf_page_max=4k,internal_page_max=4k";
    const uint64_t WiredTigerMetaData::INVALID_METADATA_IDENTIFIER = std::numeric_limits<uint64_t>::max();

    WiredTigerMetaData::WiredTigerMetaData( )
    {
        _nextId = 1;
    }

    WiredTigerMetaData::~WiredTigerMetaData()
    {
        // Free up our in-memory structure
    }

    void WiredTigerMetaData::initialize( WiredTigerDatabase &db )
    {
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
    }

    std::string WiredTigerMetaData::getName(uint64_t identifier)
    {
        WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
        invariant( itr != _tables.end() );
        return ( itr->second.name );
    }

    std::string WiredTigerMetaData::getURI(uint64_t identifier)
    {
        WiredTigerMetaDataMap::iterator itr = _tables.find(identifier);
        invariant( itr != _tables.end() );
        return ( itr->second.uri );
    }

    std::string WiredTigerMetaData::getURI(std::string name)
    {
        return getURI( getIdentifier( name ) );
   }

    uint64_t WiredTigerMetaData::getIdentifier(std::string tableName)
    {
        WiredTigerMetaDataMap::iterator itr;
        for (itr = _tables.begin(); itr != _tables.end(); itr++) {
            if (itr->second.name == tableName)
                return itr->first;
        }
        return INVALID_METADATA_IDENTIFIER;
    }

    uint64_t WiredTigerMetaData::generateIdentifier(std::string tableName, BSONObj metaData)
    {
        uint64_t old_id;

        if ( (old_id = getIdentifier(tableName)) != INVALID_METADATA_IDENTIFIER) {
            // We shouldn't ever end up with multiple tables that have matching names.
            // Check the deleted flag, and re-try a failed delete.
            //_retryTableDrop( old_id );
        }

        uint64_t id = _generateIdentifier(tableName);
        std::string uri = _generateURI(tableName, id);
        MetaDataEntry entry(tableName, uri, metaData, _isIndexName(tableName), false);
        _tables.insert( std::pair<uint64_t, MetaDataEntry>( id, entry ) );

        return id;
    }

    Status WiredTigerMetaData::remove(uint64_t identifier, bool failedDrop)
    {
        return Status::OK();
    }

    Status WiredTigerMetaData::rename(uint64_t identifier, std::string newName)
    {
        return Status::OK();
    }

    Status WiredTigerMetaData::updateMetaData(uint64_t identifier, BSONObj newMetaData)
    {
        return Status::OK();
    }

    std::map<std::string, BSONObj> WiredTigerMetaData::getAllTables()
    {
        return std::map<std::string, BSONObj>();
    }

    std::map<std::string, BSONObj> WiredTigerMetaData::getAllIndexes(std::string identifier)
    {
        return std::map<std::string, BSONObj>();
    }

    // Internal methods.
    Status WiredTigerMetaData::_populate()
    {
        uint64_t identifier, max_id;
        const char *cTableName;
        bool isIndex;
        const char *metaData;
        max_id = 0;
        while ( _metaDataCursor->next(_metaDataCursor) == 0 )
        {
            _metaDataCursor->get_key(_metaDataCursor, &identifier);
            _metaDataCursor->get_value(_metaDataCursor, &cTableName, &isIndex, &metaData);
            BSONObj b( fromjson(std::string(metaData)));

            std::string tableName(cTableName);
            std::string uri = _generateURI(tableName, identifier);
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

    // Passes in the tableName to generate different identifiers for collections and indexes
    std::string WiredTigerMetaData::_generateURI(std::string tableName, uint64_t id)
    {
        std::ostringstream uri;
        
        uri << "table:" << tableName << "_" << id;
        return uri.str();
    }

}
