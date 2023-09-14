/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/index_spec.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/read_preference.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace executor {
struct RemoteCommandResponse;
}

class DBClientCursor;

/**
 *  A handle to a remote database server backed by an individual transport::Session.
 *  This is the main entry point for talking to a simple Mongo setup.
 *
 *  In general, this type is only allowed to be used from one thread at a time. As a special
 *  exception, it is legal to call shutdownAndDisallowReconnect() from any thread as a way to
 *  interrupt the owning thread.
 */
class DBClientSession : public DBClientBase {
public:
    /**
     * A hook used to validate the reply of a "hello" command during connection. If the hook
     * returns a non-OK Status, the DBClientSession object will disconnect from the remote
     * server. This function must not throw - it can only indicate failure by returning a non-OK
     * status.
     */
    using HandshakeValidationHook =
        std::function<Status(const executor::RemoteCommandResponse& helloReply)>;

    /**
       @param autoReconnect if true, automatically reconnect on a connection failure
       @param soTimeout tcp timeout in seconds - this is for read/write, not connect.
       Connect timeout is fixed, but short, at 5 seconds.
     */
    DBClientSession(bool autoReconnect,
                    double soTimeout,
                    MongoURI uri,
                    const HandshakeValidationHook& hook,
                    const ClientAPIVersionParameters* apiParameters);

    virtual ~DBClientSession() {}

    /**
     * Connect to a Mongo database server.
     *
     * If autoReconnect is true, you can try to use the DBClientSession even when
     * connect fails -- it will try to connect again.
     *
     * @param server The server to connect to.
     */
    virtual void connect(const HostAndPort& server,
                         StringData applicationName,
                         boost::optional<TransientSSLParams> transientSSLParams);

    /**
     * This version of connect does not run "hello" after establishing a transport::Session with the
     * remote host. This method should be used only when calling "hello" would create a deadlock,
     * such as in 'isSelf'.
     *
     * @param server The server to connect to.
     */
    void connectNoHello(const HostAndPort& server,
                        boost::optional<TransientSSLParams> transientSSLParams);
    /**
     * @return true if this client is currently known to be in a failed state.  When
     * autoreconnect is on, the client will transition back to an ok state after reconnecting.
     */
    bool isFailed() const override {
        return _failed.load();
    }

    bool isStillConnected() override;

    /**
     * Disconnects the client and interrupts operations if they are currently blocked waiting for
     * the network. If autoreconnect is on, the underlying session will be re-established.
     */
    virtual void shutdown();

    /**
     * Causes an error to be reported the next time the client is used. Will interrupt
     * operations if they are currently blocked waiting for the network.
     *
     * This is the only method that is allowed to be called from other threads.
     */
    virtual void shutdownAndDisallowReconnect();

    void setWireVersions(int minWireVersion, int maxWireVersion) {
        _minWireVersion = minWireVersion;
        _maxWireVersion = maxWireVersion;
    }

    virtual int getMinWireVersion() {
        return _minWireVersion;
    }

    virtual int getMaxWireVersion() {
        return _maxWireVersion;
    }

    std::string toString() const override {
        std::stringstream ss;
        ss << _serverAddress;
        if (_failed.load())
            ss << " failed";
        return ss.str();
    }

    std::string getServerAddress() const override {
        return _serverAddress.toString();
    }
    virtual const HostAndPort& getServerHostAndPort() const {
        return _serverAddress;
    }

    void say(Message& toSend, bool isRetry = false, std::string* actualServer = nullptr) override;
    Message recv(int lastRequestId) override;

    ConnectionString::ConnectionType type() const override {
        return ConnectionString::ConnectionType::kStandalone;
    }
    void setSoTimeout(double timeout);
    double getSoTimeout() const override {
        return _socketTimeout.value_or(Milliseconds{0}).count() / 1000.0;
    }

    void setHandshakeValidationHook(const HandshakeValidationHook& hook) {
        _hook = hook;
    }

    uint64_t getSockCreationMicroSec() const override;

    MessageCompressorManager& getCompressorManager() {
        return _compressorManager;
    }

    // Throws a NetworkException if in failed state and not reconnecting or if waiting to reconnect.
    void ensureConnection() override {
        if (_failed.load()) {
            if (!_autoReconnect) {
                throwSocketError(SocketErrorKind::FAILED_STATE, toString());
            }
            _ensureSession();
        }
    }

    bool isReplicaSetMember() const override {
        return _isReplicaSetMember;
    }

    bool isMongos() const override {
        return _isMongos;
    }

    bool authenticatedDuringConnect() const override {
        return _authenticatedDuringConnect;
    }

    const MongoURI& getURI() const {
        return _uri;
    }

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() override;

    bool isUsingTransientSSLParams() const override;
#endif

protected:
    /**
     * The action to take on the underlying session after marking this client as failed.
     */
    enum FailAction {
        /**
         * Just mark the client failed, but do nothing with the session.
         */
        kSetFlag,

        /**
         * End the session after marking the client failed.
         */
        kEndSession,

        /**
         * Release ownership of the session, possibly triggering its destruction.
         * This will acquire the session lock.
         */
        kReleaseSession,

        /**
         * Shut the session down.
         * This potentially differs from simply ending the session, since it may involve cleanup
         * specific to the type of session being shut down.
         */
        kShutdownSession
    };
    void _markFailed(FailAction action);

    int _minWireVersion{0};
    int _maxWireVersion{0};
    bool _isReplicaSetMember = false;
    bool _isMongos = false;

    // The session mutex must be held to shutdown the _session from a non-owning thread, or to
    // rebind the handle from the owning thread. The thread that owns this DBClientSession is
    // allowed to use the _session without locking the mutex. This mutex also guards writes to
    // _stayFailed, although reads are allowed outside the mutex.
    Mutex _sessionMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "DBClientSession::_sessionMutex");
    std::shared_ptr<transport::Session> _session;
    boost::optional<Milliseconds> _socketTimeout;
    uint64_t _sessionCreationTimeMicros = INVALID_SOCK_CREATION_TIME;
    Date_t _lastConnectivityCheck;

    AtomicWord<bool> _stayFailed{false};
    AtomicWord<bool> _failed{false};
    const bool _autoReconnect;

    HostAndPort _serverAddress;
    std::string _applicationName;
    boost::optional<TransientSSLParams> _transientSSLParams;

private:
    Message _call(Message& toSend, std::string* actualServer) override;
    virtual StatusWith<std::shared_ptr<transport::Session>> _makeSession(
        const HostAndPort& host,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        boost::optional<TransientSSLParams> transientSSLParams = boost::none) = 0;
    virtual void _ensureSession() = 0;
    virtual void _shutdownSession() = 0;

    // Hook that is run on every call to connect()
    HandshakeValidationHook _hook;

    MessageCompressorManager _compressorManager;

    MongoURI _uri;

    bool _authenticatedDuringConnect = false;
};
}  // namespace mongo
