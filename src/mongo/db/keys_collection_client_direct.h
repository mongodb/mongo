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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/rs_local_client.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/write_concern_options.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;
class LogicalTime;
class BSONObj;

class KeysCollectionClientDirect : public KeysCollectionClient {
public:
    KeysCollectionClientDirect(bool mustUseLocalReads);

    /**
     * Returns internal keys for the given purpose and have an expiresAt value greater than
     * newerThanThis. Uses readConcern level majority if possible.
     */
    StatusWith<std::vector<KeysCollectionDocument>> getNewInternalKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        bool tryUseMajority) override;

    /**
     * Returns all external (i.e. validation-only) keys for the given purpose.
     */
    StatusWith<std::vector<ExternalKeysCollectionDocument>> getAllExternalKeys(
        OperationContext* opCtx, StringData purpose) override;

    /**
     * Directly inserts a key document to the storage
     */
    Status insertNewKey(OperationContext* opCtx, const BSONObj& doc) override;

    /**
     * Returns true if getNewKeys always uses readConcern level:local, so the documents returned can
     * be rolled back.
     */
    bool mustUseLocalReads() const final {
        return _mustUseLocalReads;
    }

private:
    /**
     * Returns keys in the given collection for the given purpose and have an expiresAt value
     * greater than newerThanThis, using readConcern level majority if possible.
     */
    template <typename KeyDocumentType>
    StatusWith<std::vector<KeyDocumentType>> _getNewKeys(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         StringData purpose,
                                                         const LogicalTime& newerThanThis,
                                                         bool tryUseMajority);

    StatusWith<Shard::QueryResponse> _query(OperationContext* opCtx,
                                            const ReadPreferenceSetting& readPref,
                                            const repl::ReadConcernLevel& readConcernLevel,
                                            const NamespaceString& nss,
                                            const BSONObj& query,
                                            const BSONObj& sort,
                                            boost::optional<long long> limit);

    Status _insert(OperationContext* opCtx,
                   const BSONObj& doc,
                   const WriteConcernOptions& writeConcern);

    RSLocalClient _rsLocalClient;
    bool _mustUseLocalReads{false};
};
}  // namespace mongo
