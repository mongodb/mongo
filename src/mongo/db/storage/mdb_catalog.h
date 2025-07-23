/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules_incompletely_marked_header.h"

#include <string>

namespace mongo {
class KVEngine;

// This is only necessary because MDBCatalogTest is defined in a .cpp file in a different module
// from MDBCatalog. Once that fixture is moved to the same module as MDBCatalog, this can be
// removed and MDBCatalogTest can be made PRIVATE.
// TODO(SERVER-105385): delete this line and comment.
class MONGO_MOD_OPEN MDBCatalogTest;

/**
 * A wrapper around the '_mdb_catalog' storage table. Each row in the table is indexed with a
 * 'RecordId', referred to as the 'Catalog ID', and contains a BSON document that describes a
 * collection's properties, indexes, and the idents which map to its storage resources.
 *
 * The 'MDBCatalog' is aware of top-level fields and idents described in each '_mdb_catalog' entry.
 * Meaningful parsing of additional 'Collection' and 'Index' properties are beyond the scope of the
 * class.
 *
 * Top-level structure of an entry in the '_mdb_catalog' catalog.
 *    {
 *      // Uniquely identifies a collection's main storage table on disk (tied to a 'RecordStore' in
 *      // the sever)
 *      'ident': <std::string>,
 *
 *      // Maps each of the collection's indexes to an ident.
 *      //      <indexName : indexIdent>
 *      // where both 'indexName' and 'indexIdent' are of type 'string'
 *      'idxIdent': <BSONObj>,
 *
 *      // Metadata field which specifies 'Collection' and 'Index' properties.
 *      'md': <BSONObj>,
 *
 *      // The namespace of the collection.
 *      'ns': <std::string>
 *    }
 */
class MDBCatalog final {
    MDBCatalog(const MDBCatalog&) = delete;
    MDBCatalog& operator=(const MDBCatalog&) = delete;
    MDBCatalog(MDBCatalog&&) = delete;
    MDBCatalog& operator=(MDBCatalog&&) = delete;

public:
    /**
     * `Entry` ties together the common identifiers of a single `_mdb_catalog` document.
     */
    struct EntryIdentifier {
        EntryIdentifier() {}
        EntryIdentifier(RecordId catalogId, std::string ident, NamespaceString nss)
            : catalogId(std::move(catalogId)), ident(std::move(ident)), nss(std::move(nss)) {}
        RecordId catalogId;
        std::string ident;
        NamespaceString nss;
    };

    MDBCatalog(RecordStore* rs, KVEngine* engine);
    MDBCatalog() = delete;

    static MDBCatalog* get(OperationContext* opCtx) {
        return opCtx->getServiceContext()->getStorageEngine()->getMDBCatalog();
    }

    void init(OperationContext* opCtx);

    /**
     * Reserves a 'catalogId' to use when creating a new catalog entry.
     */
    RecordId reserveCatalogId(OperationContext* opCtx);

    std::vector<MDBCatalog::EntryIdentifier> getAllCatalogEntries(OperationContext* opCtx) const;

    EntryIdentifier getEntry(const RecordId& catalogId) const;

    BSONObj getRawCatalogEntry(OperationContext* opCtx, const RecordId& catalogId) const;

    RecordStore::Options getParsedRecordStoreOptions(OperationContext* opCtx,
                                                     const RecordId& catalogId,
                                                     const NamespaceString& nss) const;

    void putUpdatedEntry(OperationContext* opCtx,
                         const RecordId& catalogId,
                         const BSONObj& catalogEntry);

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const;

    std::string getIndexIdent(OperationContext* opCtx,
                              const RecordId& catalogId,
                              StringData idxName) const;

    std::vector<std::string> getIndexIdents(OperationContext* opCtx, const RecordId& catalogId);

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward = true) const;

    /*
     * Adds a new 'catalogEntry' at 'catalogId' to the _mdb_catalog, but does not attempt to create
     * a backing 'RecordStore'.
     */
    StatusWith<MDBCatalog::EntryIdentifier> addEntry(OperationContext* opCtx,
                                                     const std::string& ident,
                                                     const NamespaceString& nss,
                                                     const BSONObj& catalogEntryObj,
                                                     const RecordId& catalogId);
    /**
     * Creates a new record store to back the catalog entry.
     */
    StatusWith<std::unique_ptr<RecordStore>> createRecordStoreForEntry(
        OperationContext* opCtx,
        const MDBCatalog::EntryIdentifier& entry,
        const boost::optional<UUID>& uuid,
        const RecordStore::Options& recordStoreOptions);

    StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> importCatalogEntry(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        const RecordStore::Options& recordStoreOptions,
        const BSONObj& catalogEntry,
        const BSONObj& storageMetadata,
        bool panicOnCorruptWtMetadata,
        bool repair);

    Status removeEntry(OperationContext* opCtx, const RecordId& catalogId);

    Status putRenamedEntry(OperationContext* opCtx,
                           const RecordId& catalogId,
                           const NamespaceString& toNss,
                           const BSONObj& renamedEntry);

    /**
     * First tries to return the in-memory entry. If not found, e.g. when collection is dropped
     * after the provided timestamp, loads the entry from the persisted catalog at the provided
     * timestamp.
     */
    NamespaceString getNSSFromCatalog(OperationContext* opCtx, const RecordId& catalogId) const;

    StatusWith<std::string> newOrphanedIdent(OperationContext* opCtx,
                                             const std::string& ident,
                                             bool isClustered);

    boost::optional<EntryIdentifier> getEntry_forTest(const RecordId& catalogId) const;

private:
    class AddIdentChange;

    BSONObj _findRawEntry(SeekableRecordCursor& cursor, const RecordId& catalogId) const;

    StatusWith<EntryIdentifier> _importEntry(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const BSONObj& catalogEntry);

    std::vector<std::string> _getIndexIdents(const BSONObj& rawCatalogEntry);

    // TODO SERVER-105451 refactor to avoid the need for direct bson parsing
    static RecordStore::Options _parseRecordStoreOptions(const NamespaceString& nss,
                                                         const BSONObj& obj);

    static BSONObj _buildOrphanedCatalogEntryObjAndNs(const std::string& ident,
                                                      bool isClustered,
                                                      NamespaceString* nss,
                                                      std::string* ns,
                                                      UUID uuid = UUID::gen());

    RecordStore* _rs;  // not owned

    absl::flat_hash_map<RecordId, EntryIdentifier, RecordId::Hasher> _catalogIdToEntryMap;
    mutable stdx::mutex _catalogIdToEntryMapLock;

    KVEngine* const _engine;

    friend class MDBCatalogTest;
};
}  // namespace mongo
