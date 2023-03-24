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
 * Base class modeling a gRPC ServerContext.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_server_context.html
 */
class ServerContext {
public:
    virtual ~ServerContext() {}

    /**
     * Set the server's initial metadata.
     * This must only be called before the first message is sent using the corresponding
     * ServerStream.
     *
     * This method is not thread safe with respect to ServerStream::write().
     */
    virtual void addInitialMetadataEntry(const std::string& key, const std::string& value) = 0;
    virtual const MetadataView& getClientMetadata() const = 0;
    virtual Date_t getDeadline() const = 0;
    virtual HostAndPort getHostAndPort() const = 0;

    /**
     * Attempt to cancel the RPC this context is associated with. This may not have an effect if the
     * RPC handler already returned a successful status to the client.
     *
     * This is thread-safe.
     */
    virtual void tryCancel() = 0;
    virtual bool isCancelled() const = 0;
};

}  // namespace mongo::transport::grpc
