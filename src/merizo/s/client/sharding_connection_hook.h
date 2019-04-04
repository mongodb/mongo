/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/client/connpool.h"
#include "merizo/rpc/metadata.h"
#include "merizo/s/sharding_egress_metadata_hook.h"

namespace merizo {

class DBClientBase;

/**
 * Intercepts creation of sharded connections and transparently performs the internal
 * authentication on them.
 */
class ShardingConnectionHook : public DBConnectionHook {
public:
    ShardingConnectionHook(bool shardedConnections,
                           std::unique_ptr<rpc::EgressMetadataHook> egressHook);

    void onCreate(DBClientBase* conn) override;
    void onDestroy(DBClientBase* conn) override;
    void onRelease(DBClientBase* conn) override;

private:
    bool _shardedConnections;

    // Use the implementation of the metadata readers and writers in ShardingEgressMetadataHook,
    // since that is the hook for Network Interface ASIO and this hook is to be deprecated.
    std::unique_ptr<rpc::EgressMetadataHook> _egressHook;
};

}  // namespace merizo
