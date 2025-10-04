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

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace async_rpc {

/**
 * This class serves as an adaptor that allows a mongo::RemoteCommandTargeter
 * to be used as a mongo::async_rpc::Targeter, so it can be used with the async_rpc
 * API.
 */
class AsyncRemoteCommandTargeterAdapter : public Targeter {
public:
    AsyncRemoteCommandTargeterAdapter(const ReadPreferenceSetting& readPref,
                                      std::shared_ptr<RemoteCommandTargeter> targeter)
        : _readPref(readPref), _targeter(std::move(targeter)) {}

    SemiFuture<std::vector<HostAndPort>> resolve(CancellationToken t) final {
        return _targeter->findHosts(_readPref, t);
    }

    SemiFuture<void> onRemoteCommandError(HostAndPort remoteHost,
                                          Status remoteCommandStatus) final {
        _targeter->updateHostWithStatus(remoteHost, remoteCommandStatus);
        return SemiFuture<void>::makeReady();
    }

private:
    ReadPreferenceSetting _readPref;
    std::shared_ptr<RemoteCommandTargeter> _targeter;
};

}  // namespace async_rpc
}  // namespace mongo
