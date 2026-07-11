// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
struct HostAndPort;
class OperationContext;
class Status;

namespace rpc {

/**
 * An interface for augmenting egress networking components with domain-specific metadata handling
 * logic.
 *
 * TODO: At some point we will want the opposite of this interface (readRequestMetadata,
 * writeReplyMetadata) that we will use for ingress networking. This will allow us to move much
 * of the metadata handling logic out of Command::run.
 */
class [[MONGO_MOD_OPEN]] EgressMetadataHook {
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
