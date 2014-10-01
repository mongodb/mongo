// wiredtiger_collection_catalog_entry.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <map>
#include <string>

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/json.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_collection_catalog_entry.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_database.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_metadata.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

namespace mongo {

    WiredTigerCollectionCatalogEntry::WiredTigerCollectionCatalogEntry(
        const StringData& ns, const CollectionOptions& o )
        : CollectionCatalogEntry( ns ), options( o ) {
    }

    // Constructor for a catalog entry that loads information from
    // an existing collection.
    WiredTigerCollectionCatalogEntry::WiredTigerCollectionCatalogEntry(
        WiredTigerDatabase& db, const StringData& ns, bool stayTemp)
        : CollectionCatalogEntry ( ns ), options() {

        WiredTigerMetaData &md = db.GetMetaData();
        uint64_t tblIdentifier = md.generateIdentifier( ns.toString(), options.toBSON() );
        std::string tbl_uri = md.getURI( tblIdentifier );

        // Open the WiredTiger metadata so we can retrieve saved options.
        WiredTigerSession swrap(db);
        WiredTigerCursor cursor("metadata:", swrap);
        WT_CURSOR *c = cursor.Get();
        invariant(c != NULL);
        c->set_key(c, tbl_uri.c_str());
        int ret = c->search(c);
        // TODO: Could we reasonably fail with NOTFOUND here?
        invariantWTOK(ret);
        BSONObj b = _getSavedMetadata(cursor);

        // Create the collection
        options.parse(b);
		if (!stayTemp)
			options.temp = false;
        WiredTigerRecordStore *wtRecordStore = new WiredTigerRecordStore( ns, swrap.GetDatabase() );
        if ( options.capped )
            wtRecordStore->setCapped(options.cappedSize ? options.cappedSize : 4096,
                options.cappedMaxDocs ? options.cappedMaxDocs : -1);
        rs.reset(wtRecordStore);

        // Open any existing indexes
        ret = c->next(c);
        while (ret == 0) {
            const char *uri_str;
            ret = c->get_key(c, &uri_str);
            invariantWTOK(ret);
            std::string uri(uri_str);
            // No more indexes for this table
            if (uri.substr(0, tbl_uri.size()) != tbl_uri)
                break;

            size_t pos;
            if ((pos = uri.find('$')) != std::string::npos && pos < uri.size() - 1) {
                std::string idx_name = uri.substr(pos + 1);

                b = _getSavedMetadata(cursor);
                std::string name(b.getStringField("name"));
                IndexDescriptor desc(0, "unknown", b);
                auto_ptr<IndexEntry> newEntry( new IndexEntry() );
                newEntry->name = name;
                newEntry->spec = desc.infoObj();
                // TODO: We need to stash the options field on create and decode them
                // here.
                newEntry->ready = true;
                newEntry->isMultikey = false;

                indexes[name] = newEntry.release();
            }

            ret = c->next(c);
        }
    }

    WiredTigerCollectionCatalogEntry::~WiredTigerCollectionCatalogEntry() {
        for ( Indexes::const_iterator i = indexes.begin(); i != indexes.end(); ++i )
            delete i->second;
        indexes.clear();
    }

    int WiredTigerCollectionCatalogEntry::getTotalIndexCount( OperationContext* txn ) const {
        return static_cast<int>( indexes.size() );
    }

    int WiredTigerCollectionCatalogEntry::getCompletedIndexCount( OperationContext* txn ) const {
        int ready = 0;
        for ( Indexes::const_iterator i = indexes.begin(); i != indexes.end(); ++i )
            if ( i->second->ready )
                ready++;
        return ready;
    }

    void WiredTigerCollectionCatalogEntry::getAllIndexes( OperationContext* txn, std::vector<std::string>* names ) const {
        for ( Indexes::const_iterator i = indexes.begin(); i != indexes.end(); ++i )
            names->push_back( i->second->name );
    }

    BSONObj WiredTigerCollectionCatalogEntry::getIndexSpec( OperationContext *txn, const StringData& idxName ) const {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        return i->second->spec; 
    }

    bool WiredTigerCollectionCatalogEntry::isIndexMultikey( OperationContext* txn, const StringData& idxName) const {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        return i->second->isMultikey;
    }

    bool WiredTigerCollectionCatalogEntry::setIndexIsMultikey(OperationContext* txn,
                                                              const StringData& idxName,
                                                              bool multikey ) {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        if (i->second->isMultikey == multikey)
            return false;

        i->second->isMultikey = multikey;
        return true;
    }

    DiskLoc WiredTigerCollectionCatalogEntry::getIndexHead( OperationContext* txn, const StringData& idxName ) const {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        return i->second->head;
    }

    void WiredTigerCollectionCatalogEntry::setIndexHead( OperationContext* txn,
                                                         const StringData& idxName,
                                                         const DiskLoc& newHead ) {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        i->second->head = newHead;
    }

    bool WiredTigerCollectionCatalogEntry::isIndexReady( OperationContext* txn, const StringData& idxName ) const {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        return i->second->ready;
    }

    Status WiredTigerCollectionCatalogEntry::removeIndex( OperationContext* txn,
                                                          const StringData& idxName ) {
        // XXX use a temporary session for creates: WiredTiger doesn't allow transactional creates
        // and can't deal with rolling back a creates.
        WiredTigerSession &swrap_real = WiredTigerRecoveryUnit::Get(txn).GetSession();
        WiredTigerSession swrap(swrap_real.GetDatabase());

        // Close and cached cursors
        swrap_real.GetContext().CloseAllCursors();
        swrap.GetContext().CloseAllCursors();

        WT_SESSION *session = swrap.Get();
        int ret = session->drop(session, WiredTigerIndex::_getURI(ns().toString(), idxName.toString()).c_str(), "force");
        invariantWTOK(ret);
        indexes.erase( idxName.toString() );
        return Status::OK();
    }

    Status WiredTigerCollectionCatalogEntry::prepareForIndexBuild( OperationContext* txn,
                                                                   const IndexDescriptor* spec ) {
        auto_ptr<IndexEntry> newEntry( new IndexEntry() );
        newEntry->name = spec->indexName();
        newEntry->spec = spec->infoObj();
        newEntry->ready = false;
        newEntry->isMultikey = false;

        indexes[spec->indexName()] = newEntry.release();
        return Status::OK();
    }

    void WiredTigerCollectionCatalogEntry::indexBuildSuccess( OperationContext* txn,
                                                              const StringData& idxName ) {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        i->second->ready = true;
    }

    void WiredTigerCollectionCatalogEntry::updateTTLSetting( OperationContext* txn,
                                                             const StringData& idxName,
                                                             long long newExpireSeconds ) {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );

        BSONObjBuilder b;
        for ( BSONObjIterator bi( i->second->spec ); bi.more(); ) {
            BSONElement e = bi.next();
            if ( e.fieldNameStringData() == "expireAfterSeconds" ) {
                continue;
            }
            b.append( e );
        }

        b.append( "expireAfterSeconds", newExpireSeconds );

        i->second->spec = b.obj();
    }

    bool WiredTigerCollectionCatalogEntry::indexExists( const StringData &idxName ) const {
        WiredTigerCollectionCatalogEntry::Indexes::const_iterator i = indexes.find( idxName.toString() );
        return ( i != indexes.end() );
    }

    // The cursor must be open on the metadata, and positioned on the table
    // we are retrieving the data for.
    // TODO: This belongs in WiredTigerCollectionCatalogEntry
    BSONObj WiredTigerCollectionCatalogEntry::_getSavedMetadata(WiredTigerCursor &cursor)
    {
        WT_CURSOR *c;
        c = cursor.Get();

        const char *meta;
        int ret = c->get_value(c, &meta);
        invariantWTOK(ret);
        WT_CONFIG_PARSER *cp;
        ret = wiredtiger_config_parser_open(
                NULL, meta, strlen(meta), &cp);
        invariantWTOK(ret);
        WT_CONFIG_ITEM cval;
        ret = cp->get(cp, "app_metadata", &cval);
        invariantWTOK(ret);

        BSONObj b( fromjson(std::string(cval.str, cval.len)));
        cp->close(cp);
        return b;
    }

}
