/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {

/**
 * An interface to modify the on-disk catalog metadata.
 */
class DurableCatalog {
    DurableCatalog(const DurableCatalog&) = delete;
    DurableCatalog& operator=(const DurableCatalog&) = delete;
    DurableCatalog(DurableCatalog&&) = delete;
    DurableCatalog& operator=(DurableCatalog&&) = delete;

protected:
    DurableCatalog() = default;

public:
    virtual ~DurableCatalog() {}

    static DurableCatalog* get(OperationContext* opCtx) {
        return opCtx->getServiceContext()->getStorageEngine()->getCatalog();
    }

    virtual void init(OperationContext* opCtx) = 0;

    virtual std::vector<NamespaceString> getAllCollections() const = 0;

    virtual std::string getCollectionIdent(const NamespaceString& nss) const = 0;

    virtual std::string getIndexIdent(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      StringData idName) const = 0;

    virtual BSONCollectionCatalogEntry::MetaData getMetaData(OperationContext* opCtx,
                                                             const NamespaceString& nss) const = 0;
    virtual void putMetaData(OperationContext* opCtx,
                             const NamespaceString& nss,
                             BSONCollectionCatalogEntry::MetaData& md) = 0;

    virtual std::vector<std::string> getAllIdentsForDB(StringData db) const = 0;
    virtual std::vector<std::string> getAllIdents(OperationContext* opCtx) const = 0;

    virtual bool isUserDataIdent(StringData ident) const = 0;

    virtual bool isInternalIdent(StringData ident) const = 0;

    virtual bool isCollectionIdent(StringData ident) const = 0;

    virtual RecordStore* getRecordStore() = 0;

    /**
     * Create an entry in the catalog for an orphaned collection found in the
     * storage engine. Return the generated ns of the collection.
     * Note that this function does not recreate the _id index on the collection because it does not
     * have access to index catalog.
     */
    virtual StatusWith<std::string> newOrphanedIdent(OperationContext* opCtx,
                                                     std::string ident) = 0;

    virtual std::string getFilesystemPathForDb(const std::string& dbName) const = 0;

    /**
     * Generate an internal ident name.
     */
    virtual std::string newInternalIdent() = 0;

    virtual std::unique_ptr<CollectionCatalogEntry> makeCollectionCatalogEntry(
        OperationContext* opCtx, const NamespaceString& nss, bool forRepair) = 0;

    virtual StatusWith<std::unique_ptr<CollectionCatalogEntry>> createCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& options,
        bool allocateDefaultSpace) = 0;

    virtual Status renameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromNss,
                                    const NamespaceString& toNss,
                                    bool stayTemp) = 0;

    virtual Status dropCollection(OperationContext* opCtx, const NamespaceString& nss) = 0;

    /**
     * Updates size of a capped Collection.
     */
    virtual void updateCappedSize(OperationContext* opCtx, NamespaceString ns, long long size) = 0;

    /*
     * Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
     * The specified index must already contain an expireAfterSeconds field, and the value in
     * that field and newExpireSecs must both be numeric.
     */
    virtual void updateTTLSetting(OperationContext* opCtx,
                                  NamespaceString ns,
                                  StringData idxName,
                                  long long newExpireSeconds) = 0;

    /**
     * Compare the UUID argument to the UUID obtained from the metadata. Return true if they
     * are equal, false otherwise. uuid can become a CollectionUUID once MMAPv1 is removed.
     */
    virtual bool isEqualToMetadataUUID(OperationContext* opCtx,
                                       NamespaceString ns,
                                       OptionalCollectionUUID uuid) = 0;

    /**
     * Updates the 'temp' setting for this collection.
     */
    virtual void setIsTemp(OperationContext* opCtx, NamespaceString ns, bool isTemp) = 0;

    virtual boost::optional<std::string> getSideWritesIdent(OperationContext* opCtx,
                                                            NamespaceString ns,
                                                            StringData indexName) const = 0;

    // TODO SERVER-36385 Remove this function: we don't set the feature tracker bit in 4.4 because
    // 4.4 can only downgrade to 4.2 which can read long TypeBits.
    virtual void setIndexKeyStringWithLongTypeBitsExistsOnDisk(OperationContext* opCtx) = 0;

    /**
     * Updates the validator for this collection.
     *
     * An empty validator removes all validation.
     */
    virtual void updateValidator(OperationContext* opCtx,
                                 NamespaceString ns,
                                 const BSONObj& validator,
                                 StringData validationLevel,
                                 StringData validationAction) = 0;

    virtual void updateIndexMetadata(OperationContext* opCtx,
                                     NamespaceString ns,
                                     const IndexDescriptor* desc) = 0;

    virtual Status removeIndex(OperationContext* opCtx,
                               NamespaceString ns,
                               StringData indexName) = 0;

    virtual Status prepareForIndexBuild(OperationContext* opCtx,
                                        NamespaceString ns,
                                        const IndexDescriptor* spec,
                                        IndexBuildProtocol indexBuildProtocol,
                                        bool isBackgroundSecondaryBuild) = 0;

    /**
     * Returns whether or not the index is being built with the two-phase index build procedure.
     */
    virtual bool isTwoPhaseIndexBuild(OperationContext* opCtx,
                                      NamespaceString ns,
                                      StringData indexName) const = 0;

    /**
     * Indicate that a build index is now in the "scanning" phase of a hybrid index build. The
     * 'constraintViolationsIdent' is only used for unique indexes.
     *
     * It is only valid to call this when the index is using the kTwoPhase IndexBuildProtocol.
     */
    virtual void setIndexBuildScanning(OperationContext* opCtx,
                                       NamespaceString ns,
                                       StringData indexName,
                                       std::string sideWritesIdent,
                                       boost::optional<std::string> constraintViolationsIdent) = 0;


    /**
     * Returns whether or not this index is building in the "scanning" phase.
     */
    virtual bool isIndexBuildScanning(OperationContext* opCtx,
                                      NamespaceString ns,
                                      StringData indexName) const = 0;

    /**
     * Indicate that a build index is now in the "draining" phase of a hybrid index build.
     *
     * It is only valid to call this when the index is using the kTwoPhase IndexBuildProtocol.
     */
    virtual void setIndexBuildDraining(OperationContext* opCtx,
                                       NamespaceString ns,
                                       StringData indexName) = 0;

    /**
     * Returns whether or not this index is building in the "draining" phase.
     */
    virtual bool isIndexBuildDraining(OperationContext* opCtx,
                                      NamespaceString ns,
                                      StringData indexName) const = 0;

    /**
     * Indicate that an index build is completed and the index is ready to use.
     */
    virtual void indexBuildSuccess(OperationContext* opCtx,
                                   NamespaceString ns,
                                   StringData indexName) = 0;

    /**
     * Returns true if the index identified by 'indexName' is multikey, and returns false otherwise.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index supports tracking path-level multikey information, then this function sets
     * 'multikeyPaths' as the path components that cause this index to be multikey.
     *
     * In particular, if this function returns false and the index supports tracking path-level
     * multikey information, then 'multikeyPaths' is initialized as a vector with size equal to the
     * number of elements in the index key pattern of empty sets.
     */
    virtual bool isIndexMultikey(OperationContext* opCtx,
                                 NamespaceString ns,
                                 StringData indexName,
                                 MultikeyPaths* multikeyPaths) const = 0;

    /**
     * Sets the index identified by 'indexName' to be multikey.
     *
     * If 'multikeyPaths' is non-empty, then it must be a vector with size equal to the number of
     * elements in the index key pattern. Additionally, at least one path component of the indexed
     * fields must cause this index to be multikey.
     *
     * This function returns true if the index metadata has changed, and returns false otherwise.
     */
    virtual bool setIndexIsMultikey(OperationContext* opCtx,
                                    NamespaceString ns,
                                    StringData indexName,
                                    const MultikeyPaths& multikeyPaths) = 0;

    virtual boost::optional<std::string> getConstraintViolationsIdent(
        OperationContext* opCtx, NamespaceString ns, StringData indexName) const = 0;

    /**
     * Returns the server-compatibility version of the index build procedure.
     */
    virtual long getIndexBuildVersion(OperationContext* opCtx,
                                      NamespaceString ns,
                                      StringData indexName) const = 0;

    virtual CollectionOptions getCollectionOptions(OperationContext* opCtx,
                                                   NamespaceString ns) const = 0;

    virtual int getTotalIndexCount(OperationContext* opCtx, NamespaceString ns) const = 0;

    virtual int getCompletedIndexCount(OperationContext* opCtx, NamespaceString ns) const = 0;

    virtual BSONObj getIndexSpec(OperationContext* opCtx,
                                 NamespaceString ns,
                                 StringData indexName) const = 0;

    virtual void getAllIndexes(OperationContext* opCtx,
                               NamespaceString ns,
                               std::vector<std::string>* names) const = 0;

    virtual void getReadyIndexes(OperationContext* opCtx,
                                 NamespaceString ns,
                                 std::vector<std::string>* names) const = 0;
    virtual void getAllUniqueIndexes(OperationContext* opCtx,
                                     NamespaceString ns,
                                     std::vector<std::string>* names) const = 0;

    virtual bool isIndexPresent(OperationContext* opCtx,
                                NamespaceString ns,
                                StringData indexName) const = 0;

    virtual bool isIndexReady(OperationContext* opCtx,
                              NamespaceString ns,
                              StringData indexName) const = 0;

    virtual KVPrefix getIndexPrefix(OperationContext* opCtx,
                                    NamespaceString ns,
                                    StringData indexName) const = 0;
};
}  // namespace mongo
