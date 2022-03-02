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

#include "mongo/platform/basic.h"

#include "mongo/db/vector_clock.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/vector_clock_document_gen.h"
#include "mongo/db/vector_clock_gen.h"
#include "mongo/util/static_immortal.h"

namespace mongo {
namespace {

const auto vectorClockDecoration = ServiceContext::declareDecoration<VectorClock*>();

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

VectorClock::VectorClock() = default;

VectorClock::~VectorClock() = default;

void VectorClock::registerVectorClockOnServiceContext(ServiceContext* service,
                                                      VectorClock* vectorClock) {
    invariant(!vectorClock->_service);
    vectorClock->_service = service;
    auto& clock = vectorClockDecoration(service);
    invariant(!clock);
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
             bool permitRefresh,
             BSONObjBuilder* out,
             LogicalTime time,
             Component component) const override {
        out->append(_fieldName, time.asTimestamp());
        return true;
    }

    LogicalTime in(ServiceContext* service,
                   OperationContext* opCtx,
                   const BSONObj& in,
                   bool couldBeUnauthenticated,
                   Component component) const override {
        const auto componentElem(in[_fieldName]);
        if (componentElem.eoo()) {
            // Nothing to gossip in.
            return LogicalTime();
        }
        uassert(ErrorCodes::BadValue,
                str::stream() << _fieldName << " is not a Timestamp",
                componentElem.type() == bsonTimestamp);
        return LogicalTime(componentElem.timestamp());
    }
};

class VectorClock::SignedComponentFormat : public VectorClock::ComponentFormat {
public:
    using ComponentFormat::ComponentFormat;
    virtual ~SignedComponentFormat() = default;

    bool out(ServiceContext* service,
             OperationContext* opCtx,
             bool permitRefresh,
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

            // There are some contexts where refreshing is not permitted.
            if (permitRefresh && opCtx) {
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
                   const BSONObj& in,
                   bool couldBeUnauthenticated,
                   Component component) const override {
        const auto& metadataElem = in.getField(_fieldName);
        if (metadataElem.eoo()) {
            // Nothing to gossip in.
            return LogicalTime();
        }

        const auto& obj = metadataElem.Obj();

        Timestamp ts;
        uassertStatusOK(bsonExtractTimestampField(obj, kClusterTimeFieldName, &ts));

        BSONElement signatureElem;
        uassertStatusOK(bsonExtractTypedField(obj, kSignatureFieldName, Object, &signatureElem));

        const auto& signatureObj = signatureElem.Obj();

        // Extract BinData type signature hash and construct a SHA1Block instance from it.
        BSONElement hashElem;
        uassertStatusOK(
            bsonExtractTypedField(signatureObj, kSignatureHashFieldName, BinData, &hashElem));

        int hashLength = 0;
        auto rawBinSignature = hashElem.binData(hashLength);
        BSONBinData proofBinData(rawBinSignature, hashLength, hashElem.binDataType());
        auto proofStatus = SHA1Block::fromBinData(proofBinData);
        uassertStatusOK(proofStatus);

        long long keyId;
        uassertStatusOK(bsonExtractIntegerField(signatureObj, kSignatureKeyIdFieldName, &keyId));

        auto signedTime =
            SignedLogicalTime(LogicalTime(ts), std::move(proofStatus.getValue()), keyId);

        if (!opCtx) {
            // If there's no opCtx then this must be coming from a reply, which must be internal,
            // and so doesn't require validation.
            return signedTime.getTime();
        }

        // Validate the signature.
        if (couldBeUnauthenticated && AuthorizationManager::get(service)->isAuthEnabled() &&
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
        std::make_unique<VectorClock::PlainComponentFormat>(VectorClock::kConfigTimeFieldName),
        std::make_unique<VectorClock::PlainComponentFormat>(VectorClock::kTopologyTimeFieldName)};

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

        if (opCtx && opCtx->getClient()) {
            if (auto session = opCtx->getClient()->session()) {
                return session->getTags() & transport::Session::kInternalClient;
            }
        }

        return false;
    }();

    ComponentSet toGossip = isInternal ? _gossipOutInternal() : _gossipOutExternal();

    auto now = getTime();
    bool clusterTimeWasOutput = false;
    for (auto component : toGossip) {
        clusterTimeWasOutput |= _gossipOutComponent(opCtx, outMessage, now._time, component);
    }
    return clusterTimeWasOutput;
}

void VectorClock::gossipIn(OperationContext* opCtx,
                           const BSONObj& inMessage,
                           bool couldBeUnauthenticated,
                           const transport::Session::TagMask defaultClientSessionTags) {
    if (!isEnabled()) {
        return;
    }

    auto clientSessionTags = defaultClientSessionTags;
    if (opCtx && opCtx->getClient()) {
        const auto session = opCtx->getClient()->session();
        if (session && !(session->getTags() & transport::Session::kPending)) {
            clientSessionTags = session->getTags();
        }
    }

    ComponentSet toGossip = clientSessionTags & transport::Session::kInternalClient
        ? _gossipInInternal()
        : _gossipInExternal();

    LogicalTimeArray newTime;
    for (auto component : toGossip) {
        _gossipInComponent(opCtx, inMessage, couldBeUnauthenticated, &newTime, component);
    }
    // Since the times in LogicalTimeArray are default constructed (ie. to Timestamp(0, 0)), any
    // component not present in the input BSONObj won't be advanced.
    _advanceTime(std::move(newTime));
}

bool VectorClock::_gossipOutComponent(OperationContext* opCtx,
                                      BSONObjBuilder* out,
                                      const LogicalTimeArray& time,
                                      Component component) const {
    bool wasOutput = _gossipFormatters[component]->out(
        _service, opCtx, _permitRefreshDuringGossipOut(), out, time[component], component);
    return (component == Component::ClusterTime) ? wasOutput : false;
}

void VectorClock::_gossipInComponent(OperationContext* opCtx,
                                     const BSONObj& in,
                                     bool couldBeUnauthenticated,
                                     LogicalTimeArray* newTime,
                                     Component component) {
    (*newTime)[component] =
        _gossipFormatters[component]->in(_service, opCtx, in, couldBeUnauthenticated, component);
}

std::string VectorClock::_componentName(Component component) {
    return _gossipFormatters[component]->_fieldName;
}

bool VectorClock::isEnabled() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _isEnabled;
}

void VectorClock::_disable() {
    stdx::lock_guard<Latch> lock(_mutex);
    _isEnabled = false;
}

void VectorClock::resetVectorClock_forTest() {
    stdx::lock_guard<Latch> lock(_mutex);
    auto it = _vectorTime.begin();
    for (; it != _vectorTime.end(); ++it) {
        *it = VectorClock::kInitialComponentTime;
    }
    _isEnabled = true;
}

void VectorClock::_advanceTime_forTest(Component component, LogicalTime newTime) {
    LogicalTimeArray newTimeArray;
    newTimeArray[component] = newTime;
    _advanceTime(std::move(newTimeArray));
}

}  // namespace mongo
