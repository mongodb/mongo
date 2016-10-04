// database_catalog_entry.h

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

#include <list>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"

namespace mongo {

class BSONObjBuilder;
class CollectionCatalogEntry;
class IndexAccessMethod;
class IndexCatalogEntry;
class OperationContext;
class RecordStore;

struct CollectionOptions;

class DatabaseCatalogEntry {
public:
    DatabaseCatalogEntry(StringData name) : _name(name.toString()) {}

    virtual ~DatabaseCatalogEntry() {}

    const std::string& name() const {
        return _name;
    }

    virtual bool exists() const = 0;
    virtual bool isEmpty() const = 0;
    virtual bool hasUserData() const = 0;

    virtual int64_t sizeOnDisk(OperationContext* opCtx) const = 0;

    virtual void appendExtraStats(OperationContext* opCtx,
                                  BSONObjBuilder* out,
                                  double scale) const = 0;

    // these are hacks :(
    virtual bool isOlderThan24(OperationContext* opCtx) const = 0;
    virtual void markIndexSafe24AndUp(OperationContext* opCtx) = 0;

    /**
     * Returns whethers the data files are compatible with the current code:
     *
     *   - Status::OK() if the data files are compatible with the current code.
     *
     *   - ErrorCodes::CanRepairToDowngrade if the data files are incompatible with the current
     *     code, but a --repair would make them compatible. For example, when rebuilding all indexes
     *     in the data files would resolve the incompatibility.
     *
     *   - ErrorCodes::MustUpgrade if the data files are incompatible with the current code and a
     *     newer version is required to start up.
     */
    virtual Status currentFilesCompatible(OperationContext* opCtx) const = 0;

    // ----

    virtual void getCollectionNamespaces(std::list<std::string>* out) const = 0;

    // The DatabaseCatalogEntry owns this, do not delete
    virtual CollectionCatalogEntry* getCollectionCatalogEntry(StringData ns) const = 0;

    // The DatabaseCatalogEntry owns this, do not delete
    virtual RecordStore* getRecordStore(StringData ns) const = 0;

    // Ownership passes to caller
    virtual IndexAccessMethod* getIndex(OperationContext* txn,
                                        const CollectionCatalogEntry* collection,
                                        IndexCatalogEntry* index) = 0;

    virtual Status createCollection(OperationContext* txn,
                                    StringData ns,
                                    const CollectionOptions& options,
                                    bool allocateDefaultSpace) = 0;

    virtual Status renameCollection(OperationContext* txn,
                                    StringData fromNS,
                                    StringData toNS,
                                    bool stayTemp) = 0;

    virtual Status dropCollection(OperationContext* opCtx, StringData ns) = 0;

private:
    std::string _name;
};
}
