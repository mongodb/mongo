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

#include <map>
#include <string>

#include "mongo/transport/grpc/metadata.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo::transport::grpc {

/**
 * Base class modeling a gRPC ClientContext.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_context.html
 */
class ClientContext {
public:
    virtual ~ClientContext() = default;

    /**
     * Add an entry to the metadata associated with the RPC.
     *
     * This must only be called before invoking the RPC.
     */
    virtual void addMetadataEntry(const std::string& key, const std::string& value) = 0;

    /**
     * Retrieve the server's initial metadata.
     *
     * This must only be called after the first message has been received on the ClientStream
     * created from the RPC that this context is associated with.
     */
    virtual boost::optional<const MetadataContainer&> getServerInitialMetadata() const = 0;

    /**
     * Set the deadline for the RPC to be executed using this context.
     *
     * This must only be called before invoking the RPC.
     */
    virtual void setDeadline(Date_t deadline) = 0;

    virtual Date_t getDeadline() const = 0;

    virtual HostAndPort getRemote() const = 0;

    /**
     * Send a best-effort out-of-band cancel on the call associated with this ClientContext. There
     * is no guarantee the call will be cancelled (e.g. if the call has already finished by the time
     * the cancellation is received).
     *
     * Note that tryCancel() will not impede the execution of any already scheduled work (e.g.
     * messages already queued to be sent on a stream will still be sent), though the reported
     * sucess or failure of such work may reflect the cancellation.
     *
     * This method is thread-safe, and can be called multiple times from any thread. It should not
     * be called before this ClientContext has been used to invoke an RPC.
     *
     * See:
     * https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_context.html#abd0f6715c30287b75288015eee628984
     */
    virtual void tryCancel() = 0;
};
}  // namespace mongo::transport::grpc
