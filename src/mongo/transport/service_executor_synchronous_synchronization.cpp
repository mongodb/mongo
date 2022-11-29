/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/transport/service_executor_synchronous_synchronization.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {

void SyncDomain::_findCycle(std::vector<const ThreadRecord*>& seekers) const {
    auto&& rec = *seekers.back();
    if (!rec.wants)
        return;  // Tail is not seeking anything.
    // See if any seeker in the chain, so far, holds what I want.
    for (auto&& [_, hRec] : _threads) {
        auto&& h = hRec.holds;
        if (std::find_if(h.begin(), h.end(), [&](auto&& el) { return el.name == *rec.wants; }) ==
            h.end())
            continue;
        // A node is holding what the tail of the chain wants.
        if (std::find(seekers.begin(), seekers.end(), &hRec) != seekers.end()) {
            // ... and that node is on the seeker chain.
            LOGV2_FATAL(7015116, "Deadlock", "cycle"_attr = [&] {
                BSONArrayBuilder cycle;
                for (auto&& seeker : seekers) {
                    BSONObjBuilder b{cycle.subobjStart()};
                    b.append("thread", seeker->threadName);
                    b.append("wants", *seeker->wants);
                    BSONArrayBuilder holdsArr{b.subarrayStart("holds")};
                    for (auto&& h : seeker->holds)
                        BSONObjBuilder{holdsArr.subobjStart()}
                            .append("name", h.name)
                            .append("bt", h.backtrace);
                }
                return cycle.obj();
            }());
        }
        seekers.push_back(&hRec);
        _findCycle(seekers);
        seekers.pop_back();
    }
}

}  // namespace mongo::transport
