// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * A simple class for mocking mongo::DBClientConnection.
 *
 * Also check out sample usage in dbtests/mock_dbclient_conn_test.cpp
 */
class [[MONGO_MOD_PUBLIC]] MockDBClientConnection : public mongo::DBClientConnection {
public:
    /**
     * An OP_MSG response to a 'find' command.
     */
    static Message mockFindResponse(NamespaceString nss,
                                    long long cursorId,
                                    const std::vector<BSONObj>& firstBatch,
                                    const BSONObj& metadata) {
        auto cursorRes = CursorResponse(nss, cursorId, firstBatch);
        BSONObjBuilder bob(cursorRes.toBSON(CursorResponse::ResponseType::InitialResponse));
        bob.appendElementsUnique(metadata);
        return OpMsg{bob.obj()}.serialize();
    }

    /**
     * An OP_MSG response to a 'getMore' command.
     */
    static Message mockGetMoreResponse(NamespaceString nss,
                                       long long cursorId,
                                       const std::vector<BSONObj>& batch,
                                       const BSONObj& metadata,
                                       bool moreToCome = false) {
        auto cursorRes = CursorResponse(nss, cursorId, batch);
        BSONObjBuilder bob(cursorRes.toBSON(CursorResponse::ResponseType::SubsequentResponse));
        bob.appendElementsUnique(metadata);
        auto m = OpMsg{bob.obj()}.serialize();
        if (moreToCome) {
            OpMsg::setFlag(&m, OpMsg::kMoreToCome);
        }
        return m;
    }

    /**
     * A generic non-ok OP_MSG command response.
     */
    static Message mockErrorResponse(ErrorCodes::Error err) {
        OpMsgBuilder builder;
        BSONObjBuilder bodyBob;
        bodyBob.append("ok", 0);
        bodyBob.append("code", err);
        builder.setBody(bodyBob.done());
        return builder.finish();
    }

    /**
     * Create a mock connection to a mock server.
     *
     * @param remoteServer the remote server to connect to. The caller is
     *     responsible for making sure that the life of remoteServer is
     *     longer than this connection.
     * @param autoReconnect will automatically re-establish connection the
     *     next time an operation is requested when the last operation caused
     *     this connection to fall into a failed state.
     */
    MockDBClientConnection(MockRemoteDBServer* remoteServer, bool autoReconnect = false);
    ~MockDBClientConnection() override;

    //
    // DBClientBase methods
    //
    using DBClientBase::find;

    bool connect(const char* hostName, std::string_view applicationName, std::string& errmsg);

    void connect(const HostAndPort& host,
                 std::string_view applicationName,
                 const boost::optional<TransientSSLParams>& transientSSLParams) override {
        std::string errmsg;
        if (!connect(host.toString().c_str(), applicationName, errmsg)) {
            uasserted(ErrorCodes::HostUnreachable, errmsg);
        }
    }

    using DBClientBase::runCommandWithTarget;
    std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(OpMsgRequest request) override;

    std::unique_ptr<DBClientCursor> find(FindCommandRequest findRequest,
                                         const ReadPreferenceSetting& /*unused*/,
                                         ExhaustMode /*unused*/) override;

    uint64_t getSockCreationMicroSec() const override;

    void insert(const NamespaceString& nss,
                BSONObj obj,
                bool ordered = true,
                boost::optional<BSONObj> writeConcernObj = boost::none) override;

    void insert(const NamespaceString& nss,
                const std::vector<BSONObj>& objList,
                bool ordered = true,
                boost::optional<BSONObj> writeConcernObj = boost::none) override;

    void remove(const NamespaceString& nss,
                const BSONObj& filter,
                bool removeMany = true,
                boost::optional<BSONObj> writeConcernObj = boost::none) override;

    mongo::Message recv(int lastRequestId) override;

    void shutdown() override;
    void shutdownAndDisallowReconnect() override;

    // Methods to simulate network responses.
    using Responses = std::vector<StatusWith<mongo::Message>>;
    void setCallResponses(Responses responses);
    void setRecvResponses(Responses responses);

    //
    // Getters
    //

    mongo::ConnectionString::ConnectionType type() const override;

    Message getLastSentMessage() {
        std::lock_guard lk(_netMutex);
        return _lastSentMessage;
    }

    bool isBlockedOnNetwork() {
        std::lock_guard lk(_netMutex);
        return _blockedOnNetwork;
    }

    //
    // Unsupported methods (these are pure virtuals in the base class)
    //

    void killCursor(const NamespaceString& ns, long long cursorID) override;
    void say(mongo::Message& toSend,
             bool isRetry = false,
             std::string* actualServer = nullptr) override;

private:
    mongo::Message _call(mongo::Message& toSend, std::string* actualServer) override;
    void ensureConnection() override;

    std::unique_ptr<DBClientCursor> bsonArrayToCursor(BSONArray results,
                                                      int nToSkip,
                                                      bool provideResumeToken,
                                                      int batchSize);

    MockRemoteDBServer::InstanceID _remoteServerInstanceID;
    MockRemoteDBServer* const _remoteServer;
    uint64_t _sockCreationTime;
    boost::optional<OpMsgRequest> _lastCursorMessage;

    std::mutex _netMutex;

    stdx::condition_variable _mockCallResponsesCV;
    Responses _mockCallResponses;
    Responses::iterator _callIter;

    stdx::condition_variable _mockRecvResponsesCV;
    Responses _mockRecvResponses;
    Responses::iterator _recvIter;

    Message _lastSentMessage;
    bool _blockedOnNetwork = false;
};
}  // namespace mongo
