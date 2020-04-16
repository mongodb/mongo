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

/**
 * Vector clock implementation for mongos.
 */
class VectorClockMongoS : public VectorClock {
    VectorClockMongoS(const VectorClockMongoS&) = delete;
    VectorClockMongoS& operator=(const VectorClockMongoS&) = delete;

public:
    VectorClockMongoS();
    virtual ~VectorClockMongoS();

protected:
    void _gossipOutInternal(BSONObjBuilder* out) const override;
    void _gossipOutExternal(BSONObjBuilder* out) const override;
    LogicalTimeArray _gossipInInternal(const BSONObj& in) override;
    LogicalTimeArray _gossipInExternal(const BSONObj& in) override;
};

const auto vectorClockMongoSDecoration = ServiceContext::declareDecoration<VectorClockMongoS>();

ServiceContext::ConstructorActionRegisterer _registerer(
    "VectorClockMongoS-VectorClockRegistration",
    {},
    [](ServiceContext* service) {
        VectorClockMongoS::registerVectorClockOnServiceContext(
            service, &vectorClockMongoSDecoration(service));
    },
    {});

VectorClockMongoS::VectorClockMongoS() = default;

VectorClockMongoS::~VectorClockMongoS() = default;

void VectorClockMongoS::_gossipOutInternal(BSONObjBuilder* out) const {
    VectorTime now = getTime();
    // TODO SERVER-47914: re-enable gossipping of VectorClock's ClusterTime once LogicalClock has
    // been migrated into VectorClock.
    // _gossipOutComponent(out, now, Component::ClusterTime);
    _gossipOutComponent(out, now, Component::ConfigTime);
}

void VectorClockMongoS::_gossipOutExternal(BSONObjBuilder* out) const {
    // TODO SERVER-47914: re-enable gossipping of VectorClock's ClusterTime once LogicalClock has
    // been migrated into VectorClock.
    // VectorTime now = getTime();
    // _gossipOutComponent(out, now, Component::ClusterTime);
}

VectorClock::LogicalTimeArray VectorClockMongoS::_gossipInInternal(const BSONObj& in) {
    LogicalTimeArray newTime;
    // TODO SERVER-47914: re-enable gossipping of VectorClock's ClusterTime once LogicalClock has
    // been migrated into VectorClock.
    // _gossipInComponent(in, &newTime, Component::ClusterTime);
    _gossipInComponent(in, &newTime, Component::ConfigTime);
    return newTime;
}

VectorClock::LogicalTimeArray VectorClockMongoS::_gossipInExternal(const BSONObj& in) {
    LogicalTimeArray newTime;
    // TODO SERVER-47914: re-enable gossipping of VectorClock's ClusterTime once LogicalClock has
    // been migrated into VectorClock.
    // _gossipInComponent(in, &newTime, Component::ClusterTime);
    return newTime;
}

}  // namespace
}  // namespace mongo
