/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_manager.h"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/initializer_context.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/server_parameters.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {

SimpleMutex sslManagerMtx;
SSLManagerInterface* theSSLManagerWindows = NULL;


}  // namespace

/**
 * Manage state for a SSL Connection. Used by the Socket class.
 */
class SSLConnectionWindows : public SSLConnectionInterface {
public:
    ~SSLConnectionWindows();

    std::string getSNIServerName() const final;
};


class SSLManagerWindows : public SSLManagerInterface {
public:
    explicit SSLManagerWindows(const SSLParams& params, bool isServer);

    /**
     * Initializes an OpenSSL context according to the provided settings. Only settings which are
     * acceptable on non-blocking connections are set.
     */
    Status initSSLContext(SCHANNEL_CRED* cred,
                          const SSLParams& params,
                          ConnectionDirection direction) final;

    virtual SSLConnectionInterface* connect(Socket* socket);

    virtual SSLConnectionInterface* accept(Socket* socket, const char* initialBytes, int len);

    virtual SSLPeerInfo parseAndValidatePeerCertificateDeprecated(
        const SSLConnectionInterface* conn, const std::string& remoteHost);

    StatusWith<boost::optional<SSLPeerInfo>> parseAndValidatePeerCertificate(
        PCtxtHandle ssl, const std::string& remoteHost) final;


    virtual const SSLConfiguration& getSSLConfiguration() const {
        return _sslConfiguration;
    }

    virtual int SSL_read(SSLConnectionInterface* conn, void* buf, int num);

    virtual int SSL_write(SSLConnectionInterface* conn, const void* buf, int num);

    virtual int SSL_shutdown(SSLConnectionInterface* conn);

private:
    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    SSLConfiguration _sslConfiguration;
};

// Global variable indicating if this is a server or a client instance
bool isSSLServer = false;

MONGO_INITIALIZER(SSLManager)(InitializerContext*) {
    stdx::lock_guard<SimpleMutex> lck(sslManagerMtx);
    if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
        theSSLManagerWindows = new SSLManagerWindows(sslGlobalParams, isSSLServer);
    }

    return Status::OK();
}

SSLConnectionWindows::~SSLConnectionWindows() {}

std::string SSLConnectionWindows::getSNIServerName() const {
    invariant(false);
    return "";
}

std::unique_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return stdx::make_unique<SSLManagerWindows>(params, isServer);
}

SSLManagerInterface* getSSLManager() {
    stdx::lock_guard<SimpleMutex> lck(sslManagerMtx);
    if (theSSLManagerWindows)
        return theSSLManagerWindows;
    return NULL;
}

SSLManagerWindows::SSLManagerWindows(const SSLParams& params, bool isServer)
    : _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames) {}

int SSLManagerWindows::SSL_read(SSLConnectionInterface* connInterface, void* buf, int num) {
    invariant(false);
    return 0;
}

int SSLManagerWindows::SSL_write(SSLConnectionInterface* connInterface, const void* buf, int num) {
    invariant(false);
    return 0;
}

int SSLManagerWindows::SSL_shutdown(SSLConnectionInterface* conn) {
    invariant(false);
    return 0;
}

Status SSLManagerWindows::initSSLContext(SCHANNEL_CRED* cred,
                                         const SSLParams& params,
                                         ConnectionDirection direction) {

    return Status::OK();
}

SSLConnectionInterface* SSLManagerWindows::connect(Socket* socket) {
    return nullptr;
}

SSLConnectionInterface* SSLManagerWindows::accept(Socket* socket,
                                                  const char* initialBytes,
                                                  int len) {
    return nullptr;
}

SSLPeerInfo SSLManagerWindows::parseAndValidatePeerCertificateDeprecated(
    const SSLConnectionInterface* conn, const std::string& remoteHost) {
    return SSLPeerInfo();
}

StatusWith<boost::optional<SSLPeerInfo>> SSLManagerWindows::parseAndValidatePeerCertificate(
    PCtxtHandle ssl, const std::string& remoteHost) {

    return {boost::none};
}


}  // namespace mongo
