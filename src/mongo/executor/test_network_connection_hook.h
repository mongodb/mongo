/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace executor {


/**
 * A utility for creating one-off NetworkConnectionHook instances from inline lambdas. This is
 * only to be used in testing code, not in production.
 */
template <typename ValidateFunc, typename RequestFunc, typename ReplyFunc>
class TestConnectionHook final : public NetworkConnectionHook {
public:
    TestConnectionHook(ValidateFunc&& validateFunc,
                       RequestFunc&& requestFunc,
                       ReplyFunc&& replyFunc)
        : _validateFunc(std::forward<ValidateFunc>(validateFunc)),
          _requestFunc(std::forward<RequestFunc>(requestFunc)),
          _replyFunc(std::forward<ReplyFunc>(replyFunc)) {}

    Status validateHost(const HostAndPort& remoteHost,
                        const RemoteCommandResponse& isMasterReply) override {
        return _validateFunc(remoteHost, isMasterReply);
    }

    StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(const HostAndPort& remoteHost) {
        return _requestFunc(remoteHost);
    }

    Status handleReply(const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
        return _replyFunc(remoteHost, std::move(response));
    }

private:
    ValidateFunc _validateFunc;
    RequestFunc _requestFunc;
    ReplyFunc _replyFunc;
};

/**
 * Factory function for TestConnectionHook instances. Needed for template type deduction, so that
 * one can instantiate a TestConnectionHook instance without uttering the unutterable (types).
 */
template <typename Val, typename Req, typename Rep>
std::unique_ptr<TestConnectionHook<Val, Req, Rep>> makeTestHook(Val&& validateFunc,
                                                                Req&& requestFunc,
                                                                Rep&& replyFunc) {
    return stdx::make_unique<TestConnectionHook<Val, Req, Rep>>(std::forward<Val>(validateFunc),
                                                                std::forward<Req>(requestFunc),
                                                                std::forward<Rep>(replyFunc));
}

}  // namespace executor
}  // namespace mongo
