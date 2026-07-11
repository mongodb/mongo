// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/wire_version.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <limits>
#include <new>
#include <utility>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

namespace {
auto wireSpecDecoration = ServiceContext::declareDecoration<WireSpec>();
}  // namespace

WireSpec& WireSpec::getWireSpec(ServiceContext* sc) {
    return (*sc)[wireSpecDecoration];
}

void WireSpec::appendInternalClientWireVersionIfNeeded(BSONObjBuilder* builder) {
    bool isInternalClient;
    WireVersionInfo outgoing;

    {
        std::lock_guard<std::mutex> lk(_mutex);
        fassert(9097912, isInitialized());
        isInternalClient = _spec->isInternalClient;
        outgoing = _spec->outgoing;
    }

    if (isInternalClient) {
        BSONObjBuilder subBuilder(builder->subobjStart("internalClient"));
        WireVersionInfo::appendToBSON(outgoing, &subBuilder);
    }
}

BSONObj specToBSON(const WireSpec::Specification& spec) {
    BSONObjBuilder bob;
    WireSpec::Specification::appendToBSON(spec, &bob);
    return bob.obj();
}

void WireSpec::initialize(Specification spec) {
    std::lock_guard<std::mutex> lk(_mutex);
    fassert(9097913, !isInitialized());
    BSONObj newSpec = specToBSON(spec);
    _spec = std::make_shared<Specification>(std::move(spec));
    LOGV2(4915701, "Initialized wire specification", "spec"_attr = newSpec);
}

void WireSpec::reset(Specification spec) {
    BSONObj oldSpec, newSpec;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        iassert(ErrorCodes::NotYetInitialized, "WireSpec is not yet initialized", isInitialized());

        oldSpec = specToBSON(*_spec.get());
        _spec = std::make_shared<Specification>(std::move(spec));
        newSpec = specToBSON(*_spec.get());
    }

    LOGV2(
        4915702, "Updated wire specification", "oldSpec"_attr = oldSpec, "newSpec"_attr = newSpec);
}

std::shared_ptr<const WireSpec::Specification> WireSpec::get() {
    std::lock_guard<std::mutex> lk(_mutex);
    fassert(9097914, isInitialized());
    return _spec;
}

bool WireSpec::isInternalClient() const {
    std::lock_guard<std::mutex> lk(_mutex);
    fassert(8082001, isInitialized());
    return _spec->isInternalClient;
}

WireVersionInfo WireSpec::getIncomingExternalClient() const {
    std::lock_guard<std::mutex> lk(_mutex);
    fassert(8082002, isInitialized());
    return _spec->incomingExternalClient;
}

WireVersionInfo WireSpec::getIncomingInternalClient() const {
    std::lock_guard<std::mutex> lk(_mutex);
    fassert(8082003, isInitialized());
    return _spec->incomingInternalClient;
}

WireVersionInfo WireSpec::getOutgoing() const {
    std::lock_guard<std::mutex> lk(_mutex);
    fassert(8082004, isInitialized());
    return _spec->outgoing;
}

namespace wire_version {

StatusWith<WireVersionInfo> parseWireVersionFromHelloReply(const BSONObj& helloReply) {
    long long maxWireVersion;
    auto maxWireExtractStatus =
        bsonExtractIntegerField(helloReply, "maxWireVersion", &maxWireVersion);

    long long minWireVersion;
    auto minWireExtractStatus =
        bsonExtractIntegerField(helloReply, "minWireVersion", &minWireVersion);

    // MongoDB 2.4 and earlier do not have maxWireVersion/minWireVersion in their 'isMaster'
    // replies.
    if ((maxWireExtractStatus == minWireExtractStatus) &&
        (maxWireExtractStatus == ErrorCodes::NoSuchKey)) {
        return {{0, 0}};
    } else if (!maxWireExtractStatus.isOK()) {
        return maxWireExtractStatus;
    } else if (!minWireExtractStatus.isOK()) {
        return minWireExtractStatus;
    }

    if (minWireVersion < 0 || maxWireVersion < 0 ||
        minWireVersion >= std::numeric_limits<int>::max() ||
        maxWireVersion >= std::numeric_limits<int>::max()) {
        return Status(ErrorCodes::IncompatibleServerVersion,
                      str::stream() << "Server min and max wire version have invalid values ("
                                    << minWireVersion << "," << maxWireVersion << ")");
    }

    return WireVersionInfo{static_cast<int>(minWireVersion), static_cast<int>(maxWireVersion)};
}

Status validateWireVersion(const WireVersionInfo client, const WireVersionInfo server) {
    // Since this is defined in the code, it should always hold true since this is the versions that
    // mongos/d wants to connect to.
    invariant(client.minWireVersion <= client.maxWireVersion);

    // Server may return bad data.
    if (server.minWireVersion > server.maxWireVersion) {
        return Status(ErrorCodes::IncompatibleServerVersion,
                      str::stream()
                          << "Server min and max wire version are incorrect ("
                          << server.minWireVersion << "," << server.maxWireVersion << ")");
    }

    // Determine if the [min, max] tuples overlap.
    // We assert the invariant that min < max above.
    if (!(client.minWireVersion <= server.maxWireVersion &&
          client.maxWireVersion >= server.minWireVersion)) {
        std::string errmsg = str::stream()
            << "Server min and max wire version (" << server.minWireVersion << ","
            << server.maxWireVersion << ") is incompatible with client min wire version ("
            << client.minWireVersion << "," << client.maxWireVersion << ").";
        if (client.maxWireVersion < server.minWireVersion) {
            return Status(ErrorCodes::IncompatibleWithUpgradedServer,
                          str::stream()
                              << errmsg
                              << "You (client) are attempting to connect to a node (server) that "
                                 "no longer accepts connections with your (client’s) binary "
                                 "version. Please upgrade the client’s binary version.");
        }
        return Status(ErrorCodes::IncompatibleServerVersion,
                      str::stream() << errmsg
                                    << "You (client) are attempting to connect to a node "
                                       "(server) with a binary version with which "
                                       "you (client) no longer accept connections. Please "
                                       "upgrade the server’s binary version.");
    }

    return Status::OK();
}

}  // namespace wire_version
}  // namespace mongo
