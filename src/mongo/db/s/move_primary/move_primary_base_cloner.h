/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/checked_cast.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/s/move_primary/move_primary_shared_data.h"

namespace mongo {

class MovePrimaryBaseCloner : public repl::BaseCloner {
public:
    struct CollectionParams {
        static CollectionParams parseCollectionParamsFromBSON(const BSONObj& entry);
        CollectionParams();

        CollectionParams(const NamespaceString& nss,
                         const UUID id,
                         const BSONObj& collBSONObj,
                         bool isSharded = false)
            : ns(nss), uuid(std::move(id)), collectionInfo(collBSONObj), shardedColl(isSharded) {}

        CollectionParams(const NamespaceString& nss, const UUID& id, bool isSharded = false)
            : CollectionParams(nss, id, BSONObj(), isSharded) {}

        NamespaceString ns;
        UUID uuid;
        BSONObj collectionInfo;
        bool shardedColl = false;
    };

    MovePrimaryBaseCloner(StringData clonerName,
                          MovePrimarySharedData* sharedData,
                          const HostAndPort& source,
                          DBClientConnection* client,
                          repl::StorageInterface* storageInterface,
                          ThreadPool* dbPool);
    virtual ~MovePrimaryBaseCloner() = default;

protected:
    MovePrimarySharedData* getSharedData() const override {
        return checked_cast<MovePrimarySharedData*>(BaseCloner::getSharedData());
    }

private:
    /**
     * Overriden to allow the BaseCloner to use the move primary log component.
     */
    virtual logv2::LogComponent getLogComponent() final;
};

inline bool operator<(const MovePrimaryBaseCloner::CollectionParams& left,
                      const MovePrimaryBaseCloner::CollectionParams& right) {
    return left.uuid < right.uuid;
}

}  // namespace mongo
