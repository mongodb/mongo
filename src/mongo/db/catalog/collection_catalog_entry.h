/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class Collection;
class IndexDescriptor;
class OperationContext;

// Indicates which protocol an index build is using.
enum class IndexBuildProtocol {
    /**
     * Refers to the pre-FCV 4.2 index build protocol for building indexes in replica sets.
     * Index builds must complete on the primary before replicating, and are not resumable in
     * any scenario.
     */
    kSinglePhase,
    /**
     * Refers to the FCV 4.2 two-phase index build protocol for building indexes in replica
     * sets. Indexes are built simultaneously on all nodes and are resumable during the draining
     * phase.
     */
    kTwoPhase
};

class CollectionCatalogEntry {
public:
    /**
     * Incremented when breaking changes are made to the index build procedure so that other servers
     * know whether or not to resume or discard unfinished index builds.
     */
    static const int kIndexBuildVersion = 1;

    CollectionCatalogEntry(StringData ns) : _ns(ns) {}
    virtual ~CollectionCatalogEntry() {}

    const NamespaceString& ns() const {
        return _ns;
    }

    void setNs(NamespaceString ns) {
        _ns = std::move(ns);
    }

    // ------- indexes ----------

    virtual CollectionOptions getCollectionOptions(OperationContext* opCtx) const = 0;

    virtual int getTotalIndexCount(OperationContext* opCtx) const = 0;

    virtual int getCompletedIndexCount(OperationContext* opCtx) const = 0;

    virtual int getMaxAllowedIndexes() const = 0;

    virtual void getAllIndexes(OperationContext* opCtx, std::vector<std::string>* names) const = 0;

    virtual void getReadyIndexes(OperationContext* opCtx,
                                 std::vector<std::string>* names) const = 0;

    virtual void getAllUniqueIndexes(OperationContext* opCtx,
                                     std::vector<std::string>* names) const {}

    virtual BSONObj getIndexSpec(OperationContext* opCtx, StringData idxName) const = 0;

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
                                    StringData indexName,
                                    const MultikeyPaths& multikeyPaths) = 0;

    virtual bool isIndexReady(OperationContext* opCtx, StringData indexName) const = 0;

    virtual bool isIndexPresent(OperationContext* opCtx, StringData indexName) const = 0;

    virtual KVPrefix getIndexPrefix(OperationContext* opCtx, StringData indexName) const = 0;

    virtual Status removeIndex(OperationContext* opCtx, StringData indexName) = 0;

    virtual Status prepareForIndexBuild(OperationContext* opCtx,
                                        const IndexDescriptor* spec,
                                        IndexBuildProtocol indexBuildProtocol,
                                        bool isBackgroundSecondaryBuild) = 0;

    /**
     * Returns whether or not the index is being built with the two-phase index build procedure.
     */
    virtual bool isTwoPhaseIndexBuild(OperationContext* opCtx, StringData indexName) const = 0;

    /**
     * Returns the server-compatibility version of the index build procedure.
     */
    virtual long getIndexBuildVersion(OperationContext* opCtx, StringData indexName) const = 0;

    /**
     * Indicate that a build index is now in the "scanning" phase of a hybrid index build. The
     * 'constraintViolationsIdent' is only used for unique indexes.
     *
     * It is only valid to call this when the index is using the kTwoPhase IndexBuildProtocol.
     */
    virtual void setIndexBuildScanning(OperationContext* opCtx,
                                       StringData indexName,
                                       std::string sideWritesIdent,
                                       boost::optional<std::string> constraintViolationsIdent) = 0;

    /**
     * Returns whether or not this index is building in the "scanning" phase.
     */
    virtual bool isIndexBuildScanning(OperationContext* opCtx, StringData indexName) const = 0;

    /**
     * Indicate that a build index is now in the "draining" phase of a hybrid index build.
     *
     * It is only valid to call this when the index is using the kTwoPhase IndexBuildProtocol.
     */
    virtual void setIndexBuildDraining(OperationContext* opCtx, StringData indexName) = 0;

    /**
     * Returns whether or not this index is building in the "draining" phase.
     */
    virtual bool isIndexBuildDraining(OperationContext* opCtx, StringData indexName) const = 0;

    /**
     * Indicate that an index build is completed and the index is ready to use.
     */
    virtual void indexBuildSuccess(OperationContext* opCtx, StringData indexName) = 0;

    virtual boost::optional<std::string> getSideWritesIdent(OperationContext* opCtx,
                                                            StringData indexName) const = 0;

    virtual boost::optional<std::string> getConstraintViolationsIdent(
        OperationContext* opCtx, StringData indexName) const = 0;

    /* Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
     * The specified index must already contain an expireAfterSeconds field, and the value in
     * that field and newExpireSecs must both be numeric.
     */
    virtual void updateTTLSetting(OperationContext* opCtx,
                                  StringData idxName,
                                  long long newExpireSeconds) = 0;

    virtual void updateIndexMetadata(OperationContext* opCtx, const IndexDescriptor* desc) {}

    /**
     * Updates the validator for this collection.
     *
     * An empty validator removes all validation.
     */
    virtual void updateValidator(OperationContext* opCtx,
                                 const BSONObj& validator,
                                 StringData validationLevel,
                                 StringData validationAction) = 0;

    /**
     * Updates the 'temp' setting for this collection.
     */
    virtual void setIsTemp(OperationContext* opCtx, bool isTemp) = 0;

    /**
     * Compare the UUID argument to the UUID obtained from the metadata. Return true if they
     * are equal, false otherwise. uuid can become a CollectionUUID once MMAPv1 is removed.
     */
    virtual bool isEqualToMetadataUUID(OperationContext* opCtx, OptionalCollectionUUID uuid) = 0;

    /**
     * Updates size of a capped Collection.
     */
    virtual void updateCappedSize(OperationContext* opCtx, long long size) = 0;

    // TODO SERVER-36385 Remove this function: we don't set the feature tracker bit in 4.4 because
    // 4.4 can only downgrade to 4.2 which can read long TypeBits.
    virtual void setIndexKeyStringWithLongTypeBitsExistsOnDisk(OperationContext* opCtx) = 0;

    virtual RecordStore* getRecordStore() = 0;
    virtual const RecordStore* getRecordStore() const = 0;

private:
    NamespaceString _ns;
};
}
