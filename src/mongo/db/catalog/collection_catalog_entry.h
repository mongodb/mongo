// collection_catalog_entry.h

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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"

namespace mongo {

class IndexDescriptor;
class OperationContext;

class CollectionCatalogEntry {
public:
    CollectionCatalogEntry(StringData ns) : _ns(ns) {}
    virtual ~CollectionCatalogEntry() {}

    const NamespaceString& ns() const {
        return _ns;
    }

    // ------- indexes ----------

    virtual CollectionOptions getCollectionOptions(OperationContext* txn) const = 0;

    virtual int getTotalIndexCount(OperationContext* txn) const = 0;

    virtual int getCompletedIndexCount(OperationContext* txn) const = 0;

    virtual int getMaxAllowedIndexes() const = 0;

    virtual void getAllIndexes(OperationContext* txn, std::vector<std::string>* names) const = 0;

    virtual BSONObj getIndexSpec(OperationContext* txn, StringData idxName) const = 0;

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
    virtual bool isIndexMultikey(OperationContext* txn,
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
    virtual bool setIndexIsMultikey(OperationContext* txn,
                                    StringData indexName,
                                    const MultikeyPaths& multikeyPaths) = 0;

    virtual RecordId getIndexHead(OperationContext* txn, StringData indexName) const = 0;

    virtual void setIndexHead(OperationContext* txn,
                              StringData indexName,
                              const RecordId& newHead) = 0;

    virtual bool isIndexReady(OperationContext* txn, StringData indexName) const = 0;

    virtual Status removeIndex(OperationContext* txn, StringData indexName) = 0;

    virtual Status prepareForIndexBuild(OperationContext* txn, const IndexDescriptor* spec) = 0;

    virtual void indexBuildSuccess(OperationContext* txn, StringData indexName) = 0;

    /* Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
     * The specified index must already contain an expireAfterSeconds field, and the value in
     * that field and newExpireSecs must both be numeric.
     */
    virtual void updateTTLSetting(OperationContext* txn,
                                  StringData idxName,
                                  long long newExpireSeconds) = 0;

    /**
     * Sets the flags field of CollectionOptions to newValue.
     * Subsequent calls to getCollectionOptions should have flags==newValue and flagsSet==true.
     */
    virtual void updateFlags(OperationContext* txn, int newValue) = 0;

    /**
     * Clears the "temp" flag from the CollectionOptions of this collection.
     */
    virtual void clearTempFlag(OperationContext* txn) = 0;

    /**
     * Updates the validator for this collection.
     *
     * An empty validator removes all validation.
     */
    virtual void updateValidator(OperationContext* txn,
                                 const BSONObj& validator,
                                 StringData validationLevel,
                                 StringData validationAction) = 0;

private:
    NamespaceString _ns;
};
}
