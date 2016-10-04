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

#include <wiredtiger.h>

#include "mongo/base/status_with.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

namespace mongo {

class IndexCatalogEntry;
class IndexDescriptor;
struct WiredTigerItem;

class WiredTigerIndex : public SortedDataInterface {
public:
    /**
     * Parses index options for wired tiger configuration string suitable for table creation.
     * The document 'options' is typically obtained from the 'storageEngine.wiredTiger' field
     * of an IndexDescriptor's info object.
     */
    static StatusWith<std::string> parseIndexOptions(const BSONObj& options);

    /**
     * Creates a configuration string suitable for 'config' parameter in WT_SESSION::create().
     * Configuration string is constructed from:
     *     built-in defaults
     *     'sysIndexConfig'
     *     'collIndexConfig'
     *     storageEngine.wiredTiger.configString in index descriptor's info object.
     * Performs simple validation on the supplied parameters.
     * Returns error status if validation fails.
     * Note that even if this function returns an OK status, WT_SESSION:create() may still
     * fail with the constructed configuration string.
     */
    static StatusWith<std::string> generateCreateString(const std::string& engineName,
                                                        const std::string& sysIndexConfig,
                                                        const std::string& collIndexConfig,
                                                        const IndexDescriptor& desc);

    /**
     * Creates a WiredTiger table suitable for implementing a MongoDB index.
     * 'config' should be created with generateCreateString().
     */
    static int Create(OperationContext* txn, const std::string& uri, const std::string& config);

    /**
     * @param unique - If this is a unique index.
     *                 Note: even if unique, it may be allowed ot be non-unique at times.
     */
    WiredTigerIndex(OperationContext* ctx, const std::string& uri, const IndexDescriptor* desc);

    virtual Status insert(OperationContext* txn,
                          const BSONObj& key,
                          const RecordId& id,
                          bool dupsAllowed);

    virtual void unindex(OperationContext* txn,
                         const BSONObj& key,
                         const RecordId& id,
                         bool dupsAllowed);

    virtual void fullValidate(OperationContext* txn,
                              long long* numKeysOut,
                              ValidateResults* fullResults) const;
    virtual bool appendCustomStats(OperationContext* txn,
                                   BSONObjBuilder* output,
                                   double scale) const;
    virtual Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const RecordId& id);

    virtual bool isEmpty(OperationContext* txn);

    virtual Status touch(OperationContext* txn) const;

    virtual long long getSpaceUsedBytes(OperationContext* txn) const;

    bool isDup(WT_CURSOR* c, const BSONObj& key, const RecordId& id);

    virtual Status initAsEmpty(OperationContext* txn);

    virtual Status compact(OperationContext* txn);

    const std::string& uri() const {
        return _uri;
    }

    uint64_t tableId() const {
        return _tableId;
    }
    Ordering ordering() const {
        return _ordering;
    }

    KeyString::Version keyStringVersion() const {
        return _keyStringVersion;
    }

    virtual bool unique() const = 0;

    Status dupKeyError(const BSONObj& key);

protected:
    virtual Status _insert(WT_CURSOR* c,
                           const BSONObj& key,
                           const RecordId& id,
                           bool dupsAllowed) = 0;

    virtual void _unindex(WT_CURSOR* c,
                          const BSONObj& key,
                          const RecordId& id,
                          bool dupsAllowed) = 0;

    class BulkBuilder;
    class StandardBulkBuilder;
    class UniqueBulkBuilder;

    const Ordering _ordering;
    // The keystring version is effectively const after the WiredTigerIndex instance is constructed.
    KeyString::Version _keyStringVersion;
    std::string _uri;
    uint64_t _tableId;
    std::string _collectionNamespace;
    std::string _indexName;
};


class WiredTigerIndexUnique : public WiredTigerIndex {
public:
    WiredTigerIndexUnique(OperationContext* ctx,
                          const std::string& uri,
                          const IndexDescriptor* desc);

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* txn,
                                                           bool forward) const override;

    SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn, bool dupsAllowed) override;

    bool unique() const override {
        return true;
    }

    Status _insert(WT_CURSOR* c, const BSONObj& key, const RecordId& id, bool dupsAllowed) override;

    void _unindex(WT_CURSOR* c, const BSONObj& key, const RecordId& id, bool dupsAllowed) override;
};

class WiredTigerIndexStandard : public WiredTigerIndex {
public:
    WiredTigerIndexStandard(OperationContext* ctx,
                            const std::string& uri,
                            const IndexDescriptor* desc);

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* txn,
                                                           bool forward) const override;

    SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn, bool dupsAllowed) override;

    bool unique() const override {
        return false;
    }

    Status _insert(WT_CURSOR* c, const BSONObj& key, const RecordId& id, bool dupsAllowed) override;

    void _unindex(WT_CURSOR* c, const BSONObj& key, const RecordId& id, bool dupsAllowed) override;
};

}  // namespace
