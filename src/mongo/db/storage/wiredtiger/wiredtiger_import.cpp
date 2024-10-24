/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "wiredtiger_import.h"

#include <fmt/format.h>
#include <wiredtiger.h>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/feature_document_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {

using namespace fmt::literals;

namespace {
bool shouldImport(const NamespaceString& ns, const UUID& migrationId) {
    const auto tenantId =
        tenant_migration_access_blocker::parseTenantIdFromDatabaseName(ns.dbName());

    tenant_migration_access_blocker::validateNssIsBeingMigrated(tenantId, ns, migrationId);

    return !!tenantId;
}

// catalogEntry is like {idxIdent: {myIndex: "index-12-345", myOtherIndex: "index-67-890"}}.
StringMap<std::string> makeIndexNameToIdentMap(const BSONObj& catalogEntry) {
    StringMap<std::string> indexNameToIdent;
    if (auto idxIdent = catalogEntry["idxIdent"]; idxIdent.isABSONObj()) {
        for (auto&& elt : idxIdent.Obj()) {
            indexNameToIdent[elt.fieldNameStringData()] = elt.String();
        }
    }

    return indexNameToIdent;
}

std::string _getWTMetadata(WT_SESSION* session, const std::string& uri) {
    return uassertStatusOK(WiredTigerUtil::getMetadata(session, uri));
}

std::string getTableWTMetadata(WT_SESSION* session, const std::string& ident) {
    return _getWTMetadata(session, "table:{}"_format(ident));
}

std::string getFileWTMetadata(WT_SESSION* session, const std::string& ident) {
    return _getWTMetadata(session, "file:{}.wt"_format(ident));
}

struct SizeInfo {
    long long numRecords;
    long long dataSize;
};

SizeInfo getSizeInfo(const NamespaceString& ns,
                     const std::string& ident,
                     WT_CURSOR* sizeStorerCursor) {
    const auto sizeStorerUri = "table:{}"_format(ident);
    WT_ITEM sizeStorerKey = {sizeStorerUri.c_str(), sizeStorerUri.size()};
    sizeStorerCursor->set_key(sizeStorerCursor, &sizeStorerKey);
    auto ret = sizeStorerCursor->search(sizeStorerCursor);
    if (ret != 0) {
        LOGV2_WARNING(6113803,
                      "No sizeStorer info for donor collection",
                      "ns"_attr = ns,
                      "uri"_attr = sizeStorerUri,
                      "reason"_attr = wiredtiger_strerror(ret));
        return {0, 0};
    }

    WT_ITEM item;
    uassertWTOK(sizeStorerCursor->get_value(sizeStorerCursor, &item), sizeStorerCursor->session);
    BSONObj obj{static_cast<const char*>(item.data)};
    return {obj["numRecords"].safeNumberLong(), obj["dataSize"].safeNumberLong()};
}

class CountsChange : public RecoveryUnit::Change {
public:
    CountsChange(WiredTigerRecordStore* rs, long long numRecords, long long dataSize)
        : _rs(rs), _numRecords(numRecords), _dataSize(dataSize) {}
    void commit(OperationContext* opCtx, boost::optional<Timestamp>) override {
        _rs->setNumRecords(_numRecords);
        _rs->setDataSize(_dataSize);
    }
    void rollback(OperationContext* opCtx) override {}

private:
    WiredTigerRecordStore* _rs;
    long long _numRecords;
    long long _dataSize;
};
}  // namespace

std::vector<CollectionImportMetadata> wiredTigerRollbackToStableAndGetMetadata(
    OperationContext* opCtx, const std::string& importPath, const UUID& migrationId) {
    LOGV2_DEBUG(6113400, 1, "Opening donor WiredTiger database", "importPath"_attr = importPath);
    WT_CONNECTION* conn;
    // WT converts the imported WiredTiger.backup file to a fresh WiredTiger.wt file, rolls back to
    // stable, and takes a checkpoint. Accept WT's default checkpoint behavior: take a checkpoint
    // only when opening and closing. We rely on checkpoints being disabled to make exporting the WT
    // metadata (byte offset to the root node) consistent with the new file that was written out.
    std::stringstream wtConfigBuilder;
    wtConfigBuilder
        << "log=(enabled=true,remove=true,path=journal,compressor="
        << wiredTigerGlobalOptions.journalCompressor
        << "),builtin_extension_config=(zstd=(compression_level="
        << wiredTigerGlobalOptions.zstdCompressorLevel << ")),"
        << WiredTigerExtensions::get(getGlobalServiceContext())->getOpenExtensionsConfig();
    uassertWTOK(wiredtiger_open(importPath.c_str(), nullptr, wtConfigBuilder.str().c_str(), &conn),
                nullptr);
    // Reopen as read-only, to ensure the WT metadata we retrieve will be valid after closing again.
    // Otherwise WT might change file offsets etc. between the time we get metadata and the time we
    // close conn. In fact WT doesn't do this if we don't write, but relying on explicit readonly
    // mode is better than relying implicitly on WT internals.
    uassertWTOK(conn->close(conn, nullptr), nullptr);
    wtConfigBuilder << "readonly=true";
    uassertWTOK(wiredtiger_open(importPath.c_str(), nullptr, wtConfigBuilder.str().c_str(), &conn),
                nullptr);

    ON_BLOCK_EXIT([&] {
        uassertWTOK(conn->close(conn, nullptr), nullptr);
        LOGV2_DEBUG(6113704, 1, "Closed donor WiredTiger database");
    });

    LOGV2_DEBUG(6113700, 1, "Opened donor WiredTiger database");
    WT_SESSION* session;
    uassertWTOK(conn->open_session(conn, nullptr, nullptr, &session), nullptr);
    WT_CURSOR* mdbCatalogCursor;
    WT_CURSOR* sizeStorerCursor;
    uassertWTOK(
        session->open_cursor(session, "table:_mdb_catalog", nullptr, nullptr, &mdbCatalogCursor),
        session);
    uassertWTOK(
        session->open_cursor(session, "table:sizeStorer", nullptr, nullptr, &sizeStorerCursor),
        session);

    std::vector<CollectionImportMetadata> metadatas;

    while (true) {
        opCtx->checkForInterrupt();

        int ret = mdbCatalogCursor->next(mdbCatalogCursor);
        if (ret == WT_NOTFOUND) {
            break;
        }
        uassertWTOK(ret, session);

        WT_ITEM catalogValue;
        uassertWTOK(mdbCatalogCursor->get_value(mdbCatalogCursor, &catalogValue), session);
        BSONObj rawCatalogEntry(static_cast<const char*>(catalogValue.data));

        // Skip over the version document, which doesn't correspond to a namespace entry, for
        // backwards compatibility with older versions that have a written feature document.
        if (feature_document_util::isFeatureDocument(rawCatalogEntry)) {
            continue;
        }

        NamespaceString ns(NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            rawCatalogEntry.getStringField("ns")));
        if (!shouldImport(ns, migrationId)) {
            LOGV2_DEBUG(6113801, 1, "Not importing donor collection", "ns"_attr = ns);
            continue;
        }

        auto collIdent = rawCatalogEntry["ident"].String();
        BSONCollectionCatalogEntry::MetaData catalogEntry;
        catalogEntry.parse(rawCatalogEntry["md"].Obj());
        CollectionImportMetadata collectionMetadata;
        collectionMetadata.catalogObject = rawCatalogEntry.getOwned();
        collectionMetadata.collection.ident = collIdent;
        collectionMetadata.collection.tableMetadata = getTableWTMetadata(session, collIdent);
        collectionMetadata.collection.fileMetadata = getFileWTMetadata(session, collIdent);
        collectionMetadata.ns = ns;
        auto sizeInfo = getSizeInfo(ns, collIdent, sizeStorerCursor);
        collectionMetadata.numRecords = sizeInfo.numRecords;
        collectionMetadata.dataSize = sizeInfo.dataSize;
        LOGV2_DEBUG(6113802,
                    1,
                    "Recorded collection metadata",
                    "ns"_attr = ns,
                    "ident"_attr = collectionMetadata.collection.ident,
                    "tableMetadata"_attr = collectionMetadata.collection.tableMetadata,
                    "fileMetadata"_attr = collectionMetadata.collection.fileMetadata);

        // Like: {"_id_": "/path/to/index-12-345.wt", "a_1": "/path/to/index-67-890.wt"}.
        BSONObjBuilder indexFilesBob;
        StringMap<std::string> indexNameToIdent =
            makeIndexNameToIdentMap(collectionMetadata.catalogObject);
        for (const auto& index : catalogEntry.indexes) {
            uassert(6113807,
                    "No ident for donor index '{}' in collection '{}'"_format(
                        index.nameStringData(), ns.toStringForErrorMsg()),
                    indexNameToIdent.contains(index.nameStringData()));
            uassert(6114302,
                    "Index '{}' for collection '{}' isn't ready"_format(index.nameStringData(),
                                                                        ns.toStringForErrorMsg()),
                    index.ready);

            WTIndexImportArgs indexImportArgs;
            auto indexName = index.nameStringData();
            // Ident is like "index-12-345".
            auto indexIdent = indexNameToIdent[indexName];
            indexImportArgs.indexName = indexName.toString();
            indexImportArgs.ident = indexIdent;
            indexImportArgs.tableMetadata = getTableWTMetadata(session, indexIdent);
            indexImportArgs.fileMetadata = getFileWTMetadata(session, indexIdent);
            collectionMetadata.indexes.push_back(indexImportArgs);
            LOGV2_DEBUG(6113804,
                        1,
                        "recorded index metadata",
                        "ns"_attr = ns,
                        "indexName"_attr = indexImportArgs.indexName,
                        "indexIdent"_attr = indexImportArgs.ident,
                        "tableMetadata"_attr = indexImportArgs.tableMetadata,
                        "fileMetadata"_attr = indexImportArgs.fileMetadata);
        }

        metadatas.push_back(collectionMetadata);
    }

    return metadatas;
}

std::unique_ptr<RecoveryUnit::Change> makeCountsChange(
    RecordStore* recordStore, const CollectionImportMetadata& collectionMetadata) {
    return std::make_unique<CountsChange>(checked_cast<WiredTigerRecordStore*>(recordStore),
                                          collectionMetadata.numRecords,
                                          collectionMetadata.dataSize);
}

}  // namespace mongo
