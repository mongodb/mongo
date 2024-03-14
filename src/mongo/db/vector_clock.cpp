/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <mutex>


#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/common_request_args_gen.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

const auto vectorClockCommonDecoration = ServiceContext::declareDecoration<VectorClock>();

const auto vectorClockDecoration = ServiceContext::declareDecoration<VectorClock*>();

ServiceContext::ConstructorActionRegisterer vectorClockRegisterer(
    "VectorClock", [](ServiceContext* service) {
        VectorClock::registerVectorClockOnServiceContext(service,
                                                         &vectorClockCommonDecoration(service));
    });

}  // namespace

const LogicalTime VectorClock::kInitialComponentTime{Timestamp{0, 1}};

VectorClock* VectorClock::get(ServiceContext* service) {
    return vectorClockDecoration(service);
}

VectorClock* VectorClock::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

const VectorClock* VectorClock::get(const ServiceContext* service) {
    return vectorClockDecoration(service);
}

const VectorClock* VectorClock::get(const OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void VectorClock::registerVectorClockOnServiceContext(ServiceContext* service,
                                                      VectorClock* vectorClock) {
    vectorClock->_service = service;
    auto& clock = vectorClockDecoration(service);
    clock = std::move(vectorClock);
}

VectorClock::VectorTime VectorClock::getTime() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return VectorTime(_vectorTime);
}

bool VectorClock::_lessThanOrEqualToMaxPossibleTime(LogicalTime time, uint64_t nTicks) {
    return time.asTimestamp().getSecs() <= kMaxValue &&
        time.asTimestamp().getInc() <= (kMaxValue - nTicks);
}

void VectorClock::_ensurePassesRateLimiter(ServiceContext* service,
                                           const LogicalTimeArray& newTime) {
    const unsigned wallClockSecs =
        durationCount<Seconds>(service->getFastClockSource()->now().toDurationSinceEpoch());
    auto maxAcceptableDriftSecs = static_cast<const unsigned>(gMaxAcceptableLogicalClockDriftSecs);

    for (auto newIt = newTime.begin(); newIt != newTime.end(); ++newIt) {
        auto newTimeSecs = newIt->asTimestamp().getSecs();
        auto name = _componentName(Component(newIt - newTime.begin()));

        // Both values are unsigned, so compare them first to avoid wrap-around.
        uassert(ErrorCodes::ClusterTimeFailsRateLimiter,
                str::stream() << "New " << name << ", " << newTimeSecs
                              << ", is too far from this node's wall clock time, " << wallClockSecs
                              << ".",
                ((newTimeSecs <= wallClockSecs) ||
                 (newTimeSecs - wallClockSecs) <= maxAcceptableDriftSecs));

        uassert(40484,
                str::stream() << name << " cannot be advanced beyond its maximum value",
                _lessThanOrEqualToMaxPossibleTime(*newIt, 0));
    }
}

void VectorClock::_advanceTime(LogicalTimeArray&& newTime) {
    _ensurePassesRateLimiter(_service, newTime);

    stdx::lock_guard<Latch> lock(_mutex);

    auto it = _vectorTime.begin();
    auto newIt = newTime.begin();
    for (; it != _vectorTime.end() && newIt != newTime.end(); ++it, ++newIt) {
        if (*newIt > *it) {
            *it = std::move(*newIt);
        }
    }
}

class VectorClock::PlainComponentFormat : public VectorClock::ComponentFormat {
public:
    using ComponentFormat::ComponentFormat;
    virtual ~PlainComponentFormat() = default;

    bool out(ServiceContext* service,
             OperationContext* opCtx,
             BSONObjBuilder* out,
             LogicalTime time,
             Component component) const override {
        out->append(_fieldName, time.asTimestamp());
        return true;
    }
};

LogicalTime fromOptionalTimestamp(const boost::optional<Timestamp>& time) {
    return time ? LogicalTime(*time) : LogicalTime();
}

class VectorClock::ConfigTimeComponent : public VectorClock::PlainComponentFormat {
public:
    ConfigTimeComponent() : PlainComponentFormat(VectorClock::kConfigTimeFieldName) {}
    virtual ~ConfigTimeComponent() = default;

    LogicalTime in(ServiceContext* service,
                   OperationContext* opCtx,
                   const GossipedVectorClockComponents& timepoints,
                   bool couldBeUnauthenticated,
                   Component component) const override {
        return fromOptionalTimestamp(timepoints.getDollarConfigTime());
    }
};

class VectorClock::TopologyTimeComponent : public VectorClock::PlainComponentFormat {
public:
    TopologyTimeComponent() : PlainComponentFormat(VectorClock::kTopologyTimeFieldName) {}
    virtual ~TopologyTimeComponent() = default;

    LogicalTime in(ServiceContext* service,
                   OperationContext* opCtx,
                   const GossipedVectorClockComponents& timepoints,
                   bool couldBeUnauthenticated,
                   Component component) const override {
        return fromOptionalTimestamp(timepoints.getDollarTopologyTime());
    }
};

class VectorClock::SignedComponentFormat : public VectorClock::ComponentFormat {
public:
    using ComponentFormat::ComponentFormat;
    virtual ~SignedComponentFormat() = default;

    bool out(ServiceContext* service,
             OperationContext* opCtx,
             BSONObjBuilder* out,
             LogicalTime time,
             Component component) const override {
        SignedLogicalTime signedTime;

        if (opCtx && LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
            // Authorized clients always receive a dummy-signed $clusterTime (and operationTime).
            signedTime = SignedLogicalTime(time, TimeProofService::TimeProof(), 0);
        } else {
            // Servers without validators (e.g. a shard server not yet added to a cluster) do not
            // return logical times to unauthorized clients.
            auto validator = LogicalTimeValidator::get(service);
            if (!validator) {
                return false;
            }

            // TODO (SERVER-87463): Investigate why the embedded router is not able to sign once the
            // router port is opened.
            // There are some contexts where refreshing is not permitted.
            if (opCtx && serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
                signedTime = validator->signLogicalTime(opCtx, time);
            } else {
                signedTime = validator->trySignLogicalTime(time);
            }

            // If there were no keys, do not return $clusterTime (or operationTime) to unauthorized
            // clients.
            if (signedTime.getKeyId() == 0) {
                return false;
            }
        }

        BSONObjBuilder subObjBuilder(out->subobjStart(_fieldName));
        signedTime.getTime().asTimestamp().append(subObjBuilder.bb(), kClusterTimeFieldName);

        BSONObjBuilder signatureObjBuilder(subObjBuilder.subobjStart(kSignatureFieldName));
        // Cluster time metadata is only written when the LogicalTimeValidator is set, which
        // means the cluster time should always have a proof.
        invariant(signedTime.getProof());
        signedTime.getProof()->appendAsBinData(signatureObjBuilder, kSignatureHashFieldName);
        signatureObjBuilder.append(kSignatureKeyIdFieldName, signedTime.getKeyId());
        signatureObjBuilder.doneFast();

        subObjBuilder.doneFast();

        return true;
    }

    LogicalTime in(ServiceContext* service,
                   OperationContext* opCtx,
                   const GossipedVectorClockComponents& timepoints,
                   bool couldBeUnauthenticated,
                   Component component) const override {
        if (!timepoints.getDollarClusterTime()) {
            // Nothing to gossip in.
            return LogicalTime();
        }

        auto& clusterTime = *(timepoints.getDollarClusterTime());
        Timestamp ts = clusterTime.getClusterTime();

        auto hashCDR = clusterTime.getSignature()->getHash();
        auto hashLength = hashCDR.length();
        auto rawBinSignature = reinterpret_cast<const unsigned char*>(hashCDR.data());
        BSONBinData proofBinData(rawBinSignature, hashLength, BinDataType::BinDataGeneral);
        auto proofStatus = SHA1Block::fromBinData(proofBinData);

        long long keyId = clusterTime.getSignature()->getKeyId();

        auto signedTime =
            SignedLogicalTime(LogicalTime(ts), std::move(proofStatus.getValue()), keyId);

        if (!opCtx) {
            // If there's no opCtx then this must be coming from a reply, which must be internal,
            // and so doesn't require validation.
            return signedTime.getTime();
        }

        // Validate the signature.
        if (couldBeUnauthenticated &&
            AuthorizationManager::get(opCtx->getService())->isAuthEnabled() &&
            (!signedTime.getProof() || *signedTime.getProof() == TimeProofService::TimeProof())) {

            AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
            // The client is not authenticated and is not using localhost auth bypass. Do not
            // gossip.
            if (authSession && !authSession->isAuthenticated() &&
                !authSession->isUsingLocalhostBypass()) {
                return {};
            }
        }

        auto logicalTimeValidator = LogicalTimeValidator::get(service);
        if (!LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
            if (!logicalTimeValidator) {
                uasserted(ErrorCodes::CannotVerifyAndSignLogicalTime,
                          "Cannot accept logicalTime: " + signedTime.getTime().toString() +
                              ". May not be a part of a sharded cluster");
            } else {
                uassertStatusOK(logicalTimeValidator->validate(opCtx, signedTime));
            }
        }

        return signedTime.getTime();
    }

private:
    static constexpr char kClusterTimeFieldName[] = "clusterTime";
    static constexpr char kSignatureFieldName[] = "signature";
    static constexpr char kSignatureHashFieldName[] = "hash";
    static constexpr char kSignatureKeyIdFieldName[] = "keyId";
};

const VectorClock::ComponentArray<std::unique_ptr<VectorClock::ComponentFormat>>
    VectorClock::_gossipFormatters{
        std::make_unique<VectorClock::SignedComponentFormat>(VectorClock::kClusterTimeFieldName),
        std::make_unique<VectorClock::ConfigTimeComponent>(),
        std::make_unique<VectorClock::TopologyTimeComponent>()};

bool VectorClock::gossipOut(OperationContext* opCtx,
                            BSONObjBuilder* outMessage,
                            bool forceInternal) const {
    if (!isEnabled()) {
        return false;
    }

    const auto isInternal = [&]() -> bool {
        if (forceInternal) {
            return true;
        }

        if (opCtx) {
            if (auto client = opCtx->getClient(); client && client->session()) {
                return opCtx->getClient()->isInternalClient();
            }
        }

        return false;
    }();

    ComponentSet toGossip =
        isInternal ? _getGossipInternalComponents() : _getGossipExternalComponents();

    auto now = getTime();
    bool clusterTimeWasOutput = false;
    for (auto component : toGossip) {
        clusterTimeWasOutput |= _gossipOutComponent(opCtx, outMessage, now._time, component);
    }
    return clusterTimeWasOutput;
}

void VectorClock::gossipIn(OperationContext* opCtx,
                           const GossipedVectorClockComponents& timepoints,
                           bool couldBeUnauthenticated,
                           bool defaultIsInternalClient) {
    if (!isEnabled()) {
        return;
    }

    auto isInternal = defaultIsInternalClient;
    if (opCtx) {
        if (const auto client = opCtx->getClient()) {
            if (client->session() && !(client->getTags() & Client::kPending)) {
                isInternal = client->isInternalClient();
            }
        }
    }

    ComponentSet toGossip =
        isInternal ? _getGossipInternalComponents() : _getGossipExternalComponents();

    LogicalTimeArray newTime;
    for (auto component : toGossip) {
        _gossipInComponent(opCtx, timepoints, couldBeUnauthenticated, &newTime, component);
    }
    // Since the times in LogicalTimeArray are default constructed (ie. to Timestamp(0, 0)), any
    // component not present in the input BSONObj won't be advanced.
    _advanceTime(std::move(newTime));
}

bool VectorClock::_gossipOutComponent(OperationContext* opCtx,
                                      BSONObjBuilder* out,
                                      const LogicalTimeArray& time,
                                      Component component) const {
    bool wasOutput =
        _gossipFormatters[component]->out(_service, opCtx, out, time[component], component);
    return (component == Component::ClusterTime) ? wasOutput : false;
}

void VectorClock::_gossipInComponent(OperationContext* opCtx,
                                     const GossipedVectorClockComponents& timepoints,
                                     bool couldBeUnauthenticated,
                                     LogicalTimeArray* newTime,
                                     Component component) {
    (*newTime)[component] = _gossipFormatters[component]->in(
        _service, opCtx, timepoints, couldBeUnauthenticated, component);
}

std::string VectorClock::_componentName(Component component) {
    return _gossipFormatters[component]->_fieldName;
}

bool VectorClock::isEnabled() const {
    return _isEnabled.load();
}

void VectorClock::_disable() {
    _isEnabled.store(false);
}

void VectorClock::resetVectorClock_forTest() {
    stdx::lock_guard<Latch> lock(_mutex);
    auto it = _vectorTime.begin();
    for (; it != _vectorTime.end(); ++it) {
        *it = VectorClock::kInitialComponentTime;
    }
    _isEnabled.store(true);
}

void VectorClock::_advanceTime_forTest(Component component, LogicalTime newTime) {
    LogicalTimeArray newTimeArray;
    newTimeArray[component] = newTime;
    _advanceTime(std::move(newTimeArray));
}

bool VectorClock::_permitGossipClusterTimeWithExternalClients() const {
    // Permit gossiping with external clients in case this node is a standalone mongos.
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return true;
    }

    // If this node has no replication coordinator, permit gossiping with external clients. On
    // the other hand, if this node has replication coordinator but it is in an unreadable state,
    // skip gossiping because it may require reading a signing key from the keys collection.
    auto replicationCoordinator = repl::ReplicationCoordinator::get(_service);
    return !replicationCoordinator ||
        (replicationCoordinator->getSettings().isReplSet() &&
         // Check repl status without locks to prevent deadlocks. This is a best effort check
         // as the repl state can change right after this check even when inspected under a
         // lock or mutex.
         replicationCoordinator->isInPrimaryOrSecondaryState_UNSAFE());
}

}  // namespace mongo
