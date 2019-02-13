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

#include "mongo/db/service_context.h"
#include "mongo/executor/egress_tag_closer.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/transport/session.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace executor {

/**
 * Manager for some number of EgressTagClosers, controlling dispatching to the managed resources.
 *
 * The idea is that you own some semi-global EgressTagCloserManager which owns a bunch of TagClosers
 * (which register themselves with it) and then interact exclusively with the manager.
 */
class EgressTagCloserManager {
public:
    EgressTagCloserManager() = default;

    static EgressTagCloserManager& get(ServiceContext* svc);

    void add(EgressTagCloser* etc);
    void remove(EgressTagCloser* etc);

    void dropConnections(transport::Session::TagMask tags);

    void dropConnections(const HostAndPort& hostAndPort);

    void mutateTags(
        const HostAndPort& hostAndPort,
        const stdx::function<transport::Session::TagMask(transport::Session::TagMask)>& mutateFunc);

private:
    stdx::mutex _mutex;
    stdx::unordered_set<EgressTagCloser*> _egressTagClosers;
};

}  // namespace executor
}  // namespace mongo
