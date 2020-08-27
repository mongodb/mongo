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

namespace mongo {
namespace {

class VectorClockMongoS : public VectorClock {
    VectorClockMongoS(const VectorClockMongoS&) = delete;
    VectorClockMongoS& operator=(const VectorClockMongoS&) = delete;

public:
    VectorClockMongoS();
    virtual ~VectorClockMongoS();

private:
    // VectorClock methods implementation

    ComponentSet _gossipInInternal() const override;
    ComponentSet _gossipOutInternal() const override;

    bool _permitRefreshDuringGossipOut() const override {
        return true;
    }
};

const auto vectorClockMongoSDecoration = ServiceContext::declareDecoration<VectorClockMongoS>();

ServiceContext::ConstructorActionRegisterer vectorClockMongoSRegisterer(
    "VectorClockMongoS-VectorClockRegistration",
    {},
    [](ServiceContext* service) {
        VectorClockMongoS::registerVectorClockOnServiceContext(
            service, &vectorClockMongoSDecoration(service));
    },
    {});

VectorClockMongoS::VectorClockMongoS() = default;

VectorClockMongoS::~VectorClockMongoS() = default;

VectorClock::ComponentSet VectorClockMongoS::_gossipOutInternal() const {
    return VectorClock::ComponentSet{
        Component::ClusterTime, Component::ConfigTime, Component::TopologyTime};
}

VectorClock::ComponentSet VectorClockMongoS::_gossipInInternal() const {
    return VectorClock::ComponentSet{
        Component::ClusterTime, Component::ConfigTime, Component::TopologyTime};
}

}  // namespace
}  // namespace mongo
