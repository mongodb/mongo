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

#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/keys_collection_document_gen.h"

namespace mongo {

class BSONObj;
class LogicalTime;
class OperationContext;

class KeysCollectionClient {
public:
    virtual ~KeysCollectionClient() = default;

    /**
     * Returns internal keys (keys for signing and validating cluster times created by nodes in the
     * clusters that this node is in) that match the given purpose and have an expiresAt value
     * greater than newerThanThis. Uses readConcern level majority if possible.
     */
    virtual StatusWith<std::vector<KeysCollectionDocument>> getNewInternalKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        bool useMajority) = 0;

    /**
     * Returns all external keys (validation-only keys copied from other clusters) that match the
     * given purpose.
     */
    virtual StatusWith<std::vector<ExternalKeysCollectionDocument>> getAllExternalKeys(
        OperationContext* opCtx, StringData purpose) = 0;

    /**
     * Directly inserts a key document to the storage
     */
    virtual Status insertNewKey(OperationContext* opCtx, const BSONObj& doc) = 0;

    /**
     * Returns true if it performs majority reads
     */
    virtual bool supportsMajorityReads() const = 0;
};

}  // namespace mongo
