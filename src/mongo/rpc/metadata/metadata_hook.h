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

namespace mongo {

class BSONObj;
class BSONObjBuilder;
struct HostAndPort;
class OperationContext;
class Status;
class StringData;

namespace rpc {

/**
 * An interface for augmenting egress networking components with domain-specific metadata handling
 * logic.
 *
 * TODO: At some point we will want the opposite of this interface (readRequestMetadata,
 * writeReplyMetadata) that we will use for ingress networking. This will allow us to move much
 * of the metadata handling logic out of Command::run.
 */
class EgressMetadataHook {
public:
    virtual ~EgressMetadataHook() = default;

    /**
     * Writes to an outgoing request metadata object. This method must not throw or block on
     * database or network operations and can be called by multiple concurrent threads.
     *
     * opCtx may be null as writeRequestMetadata may be called on ASIO background threads, and may
     * not
     * have an OperationContext as a result.
     */
    virtual Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) = 0;

    /**
     * Reads metadata from an incoming command reply. This method must not throw or block on
     * database or network operations and can be called by multiple concurrent threads.
     */
    virtual Status readReplyMetadata(OperationContext* opCtx, const BSONObj& metadataObj) = 0;

protected:
    EgressMetadataHook() = default;
};

}  // namespace rpc
}  // namespace mongo
