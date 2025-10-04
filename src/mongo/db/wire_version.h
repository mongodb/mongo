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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/version/releases.h"

#include <memory>
#include <string>

#include <boost/optional.hpp>

namespace mongo {

/**
 * The 'WireVersion' captures all "protocol events" the write protocol went through.  A
 * protocol event is a change in the syntax of messages on the wire or the semantics of
 * existing messages. We may also add "logical" entries for releases, although that's not
 * mandatory.
 *
 * We use the wire version to determine if two agents (a driver, a mongos, or a mongod) can
 * interact. Each agent carries two versions, a 'max' and a 'min' one. If the two agents
 * are on the same 'max' number, they stricly speak the same wire protocol and it is safe
 * to allow them to communicate. If two agents' ranges do not intersect, they should not be
 * allowed to communicate.
 *
 * If two agents have at least one version in common they can communicate, but one of the
 * sides has to be ready to compensate for not being on its partner version.
 */
enum WireVersion {
    // Everything before we started tracking.
    RELEASE_2_4_AND_BEFORE = 0,

    // The aggregation command may now be requested to return cursors.
    AGG_RETURNS_CURSORS = 1,

    // insert, update, and delete batch command
    BATCH_COMMANDS = 2,

    // support SCRAM-SHA1, listIndexes, listCollections, new explain
    RELEASE_2_7_7 = 3,

    // Support find and getMore commands, as well as OP_COMMAND in mongod (but not mongos).
    FIND_COMMAND = 4,

    // Supports all write commands take a write concern.
    COMMANDS_ACCEPT_WRITE_CONCERN = 5,

    // Supports the new OP_MSG wireprotocol (3.6+).
    SUPPORTS_OP_MSG = 6,

    // Supports replica set transactions (4.0+).
    REPLICA_SET_TRANSACTIONS = 7,

    // Supports sharded transactions (4.2+).
    SHARDED_TRANSACTIONS = 8,

    // Supports resumable initial sync (4.4+).
    RESUMABLE_INITIAL_SYNC = 9,

    // Supports features available from 4.7 and onwards.
    WIRE_VERSION_47 = 10,

    // Supports features available from 4.8 and onwards.
    WIRE_VERSION_48 = 11,

    // Supports features available from 4.9 and onwards.
    WIRE_VERSION_49 = 12,

    // Supports features available from 5.0 and onwards.
    WIRE_VERSION_50 = 13,

    // Supports features available from 5.1 and onwards.
    WIRE_VERSION_51 = 14,

    // Calculate the latest wire version using the number of releases since 4.0.
    LATEST_WIRE_VERSION = REPLICA_SET_TRANSACTIONS + multiversion::kSince_4_0,

    // Set this to LATEST_WIRE_VERSION - 1.
    LAST_CONT_WIRE_VERSION = LATEST_WIRE_VERSION - 1,

    // Calculate the last LTS wire version using the latest wire version minus the number of
    // releases since last LTS.
    LAST_LTS_WIRE_VERSION = LATEST_WIRE_VERSION - multiversion::kSinceLastLTS,
};

/**
 * Struct to pass around information about wire version.
 */
struct WireVersionInfo {
    int minWireVersion;
    int maxWireVersion;

    static void appendToBSON(const WireVersionInfo& versionInfo, BSONObjBuilder* bob) {
        bob->append("minWireVersion", versionInfo.minWireVersion);
        bob->append("maxWireVersion", versionInfo.maxWireVersion);
    }
};

class WireSpec {
public:
    static WireSpec& getWireSpec(ServiceContext* sc);

    struct Specification {
        // incomingExternalClient.minWireVersion - Minimum version that the server accepts on
        // incoming requests from external clients. We should bump this whenever we don't want to
        // allow incoming connections from clients that are too old.

        // incomingExternalClient.maxWireVersion - Latest version that the server accepts on
        // incoming requests from external clients. This should always be at the latest entry in
        // WireVersion.
        WireVersionInfo incomingExternalClient = {RELEASE_2_4_AND_BEFORE, LATEST_WIRE_VERSION};

        // incomingInternalClient.minWireVersion - Minimum version that the server accepts on
        // incoming requests from internal clients. This should be
        // incomingInternalClient.maxWireVersion - 1, when the featureCompatibilityVersion is equal
        // to the downgrade version, and incomingInternalClient.maxWireVersion otherwise. However,
        // in 3.6, this needs to be RELEASE_2_4_AND_BEFORE when the featureCompatibilityVersion is
        // equal to the downgrade version due to a bug in 3.4, where if the receiving node says it
        // supports wire version range [COMMANDS_ACCEPT_WRITE_CONCERN, SUPPORTS_OP_MSG] and it is a
        // mongod, the initiating node will think it only supports OP_QUERY.

        // incomingInternalClient.maxWireVersion - Latest version that the server accepts on
        // incoming requests. This should always be at the latest entry in WireVersion.
        WireVersionInfo incomingInternalClient = {RELEASE_2_4_AND_BEFORE, LATEST_WIRE_VERSION};

        // outgoing.minWireVersion - Minimum version allowed on remote nodes when the server sends
        // requests. This should be outgoing.maxWireVersion - 1, when the
        // featureCompatibilityVersion is equal to the downgrade version, and
        // outgoing.maxWireVersion otherwise. However, in 3.6, this needs to be
        // RELEASE_2_4_AND_BEFORE when the featureCompatibilityVersion is equal to the downgrade
        // version due to a bug in 3.4, where if the receiving node says it supports wire version
        // range [COMMANDS_ACCEPT_WRITE_CONCERN, SUPPORTS_OP_MSG] and it is a mongod, the initiating
        // node will think it only supports OP_QUERY.

        // outgoing.maxWireVersion - Latest version allowed on remote nodes when the server sends
        // requests.
        WireVersionInfo outgoing = {SUPPORTS_OP_MSG, LATEST_WIRE_VERSION};

        // Set to true if the client is internal to the cluster---this is a mongod or mongos
        // connecting to another mongod.
        bool isInternalClient = false;

        static void appendToBSON(const Specification& spec, BSONObjBuilder* bob) {
            auto appendWireVersion = [bob](StringData tag, const WireVersionInfo& wireVersionInfo) {
                BSONObjBuilder builder = bob->subobjStart(tag);
                WireVersionInfo::appendToBSON(wireVersionInfo, &builder);
            };

            appendWireVersion("incomingExternalClient"_sd, spec.incomingExternalClient);
            appendWireVersion("incomingInternalClient"_sd, spec.incomingInternalClient);
            appendWireVersion("outgoing"_sd, spec.outgoing);

            bob->append("isInternalClient", spec.isInternalClient);
        }
    };

public:
    /**
     * Appends the min and max versions in 'wireVersionInfo' to 'builder' in the format expected for
     * reporting information about the internal client, if the WireSpec represents the internal
     * client.
     *
     * Intended for use as part of performing the isMaster/hello handshake with a remote node. When
     * an internal clients make a connection to another node in the cluster, it includes internal
     * client information as a parameter to the hello command. This parameter has the following
     * format:
     *
     *    internalClient: {
     *        minWireVersion: <int>,
     *        maxWireVersion: <int>
     *    }
     *
     * This information can be used to ensure correctness during upgrade in mixed version clusters.
     */
    void appendInternalClientWireVersionIfNeeded(BSONObjBuilder* builder);

    void initialize(Specification spec);

    void reset(Specification spec);

    // Calling `get()` on uninitialized instances of `WireSpec` is an invariant failure.
    std::shared_ptr<const Specification> get();

    // These getters allow direct access to fields in `WireSpec::Specification.`
    // Calling these getters on uninitialized instances of `WireSpec` is an invariant failure.
    WireVersionInfo getIncomingExternalClient() const;
    WireVersionInfo getIncomingInternalClient() const;
    WireVersionInfo getOutgoing() const;
    bool isInternalClient() const;

private:
    // Ensures concurrent accesses to `get()`, `appendInternalClientWireVersionIfNeeded()`, and
    // `reset()` are thread-safe.
    mutable stdx::mutex _mutex;

    std::shared_ptr<const Specification> _spec;

    bool isInitialized() const {
        return !!_spec;
    }
};

namespace wire_version {

/**
 * Validates client and server wire version. The server's wire version is returned from
 * hello/isMaster, and the client is from WireSpec.instance().
 */
Status validateWireVersion(WireVersionInfo client, WireVersionInfo server);

/**
 * Determines the min/max wire version of a remote server from a hello/isMaster command reply.
 */
StatusWith<WireVersionInfo> parseWireVersionFromHelloReply(const BSONObj& helloReply);

}  // namespace wire_version
}  // namespace mongo
