/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#pragma once

#include <memory>

#include "mongo/db/service_context.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/future.h"

namespace mongo {

class AsyncDBClient : public std::enable_shared_from_this<AsyncDBClient> {
public:
    explicit AsyncDBClient(const HostAndPort& peer,
                           transport::SessionHandle session,
                           ServiceContext* svcCtx)
        : _peer(std::move(peer)), _session(std::move(session)), _svcCtx(svcCtx) {}

    using Handle = std::shared_ptr<AsyncDBClient>;

    static Future<Handle> connect(const HostAndPort& peer,
                                  transport::ConnectSSLMode sslMode,
                                  ServiceContext* const context,
                                  transport::ReactorHandle reactor,
                                  Milliseconds timeout);

    Future<executor::RemoteCommandResponse> runCommandRequest(
        executor::RemoteCommandRequest request, const transport::BatonHandle& baton = nullptr);
    Future<rpc::UniqueReply> runCommand(OpMsgRequest request,
                                        const transport::BatonHandle& baton = nullptr);

    Future<void> authenticate(const BSONObj& params);

    Future<void> initWireVersion(const std::string& appName,
                                 executor::NetworkConnectionHook* const hook);

    void cancel(const transport::BatonHandle& baton = nullptr);

    bool isStillConnected();

    void end();

    const HostAndPort& remote() const;
    const HostAndPort& local() const;

private:
    Future<Message> _call(Message request, const transport::BatonHandle& baton = nullptr);
    BSONObj _buildIsMasterRequest(const std::string& appName);
    void _parseIsMasterResponse(BSONObj request,
                                const std::unique_ptr<rpc::ReplyInterface>& response);

    const HostAndPort _peer;
    transport::SessionHandle _session;
    ServiceContext* const _svcCtx;
    MessageCompressorManager _compressorManager;
    boost::optional<rpc::Protocol> _negotiatedProtocol;
};

}  // namespace mongo
