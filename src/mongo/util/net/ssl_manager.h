/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <memory>
#include <string>

#include "mongo/config.h"

#ifdef MONGO_CONFIG_SSL

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/time_support.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#endif  // #ifdef MONGO_CONFIG_SSL

namespace mongo {
/*
 * @return the SSL version std::string prefixed with prefix and suffixed with suffix
 */
const std::string getSSLVersion(const std::string& prefix, const std::string& suffix);
}

#ifdef MONGO_CONFIG_SSL
namespace mongo {
struct SSLParams;

class SSLConnection {
public:
    SSL* ssl;
    BIO* networkBIO;
    BIO* internalBIO;
    Socket* socket;

    SSLConnection(SSL_CTX* ctx, Socket* sock, const char* initialBytes, int len);

    ~SSLConnection();
};

struct SSLConfiguration {
    SSLConfiguration() : serverSubjectName(""), clientSubjectName("") {}
    SSLConfiguration(const std::string& serverSubjectName,
                     const std::string& clientSubjectName,
                     const Date_t& serverCertificateExpirationDate)
        : serverSubjectName(serverSubjectName),
          clientSubjectName(clientSubjectName),
          serverCertificateExpirationDate(serverCertificateExpirationDate) {}

    bool isClusterMember(StringData subjectName) const;
    BSONObj getServerStatusBSON() const;
    std::string serverSubjectName;
    std::string clientSubjectName;
    Date_t serverCertificateExpirationDate;
    bool hasCA = false;
};

class SSLManagerInterface : public Decorable<SSLManagerInterface> {
public:
    static std::unique_ptr<SSLManagerInterface> create(const SSLParams& params, bool isServer);

    virtual ~SSLManagerInterface();

    /**
     * Initiates a TLS connection.
     * Throws SocketException on failure.
     * @return a pointer to an SSLConnection. Resources are freed in SSLConnection's destructor
     */
    virtual SSLConnection* connect(Socket* socket) = 0;

    /**
     * Waits for the other side to initiate a TLS connection.
     * Throws SocketException on failure.
     * @return a pointer to an SSLConnection. Resources are freed in SSLConnection's destructor
     */
    virtual SSLConnection* accept(Socket* socket, const char* initialBytes, int len) = 0;

    /**
     * Fetches a peer certificate and validates it if it exists
     * Throws SocketException on failure
     * @return a std::string containing the certificate's subject name.
     *
     * This version of parseAndValidatePeerCertificate is deprecated because it throws a
     * SocketException upon failure. New code should prefer the version that returns
     * a StatusWith instead.
     */
    virtual std::string parseAndValidatePeerCertificateDeprecated(
        const SSLConnection* conn, const std::string& remoteHost) = 0;

    /**
     * Gets the SSLConfiguration containing all information about the current SSL setup
     * @return the SSLConfiguration
     */
    virtual const SSLConfiguration& getSSLConfiguration() const = 0;

    /**
    * Fetches the error text for an error code, in a thread-safe manner.
    */
    static std::string getSSLErrorMessage(int code);

    /**
     * ssl.h wrappers
     */
    virtual int SSL_read(SSLConnection* conn, void* buf, int num) = 0;

    virtual int SSL_write(SSLConnection* conn, const void* buf, int num) = 0;

    virtual unsigned long ERR_get_error() = 0;

    virtual char* ERR_error_string(unsigned long e, char* buf) = 0;

    virtual int SSL_get_error(const SSLConnection* conn, int ret) = 0;

    virtual int SSL_shutdown(SSLConnection* conn) = 0;

    virtual void SSL_free(SSLConnection* conn) = 0;

    enum class ConnectionDirection { kIncoming, kOutgoing };

    /**
     * Initializes an OpenSSL context according to the provided settings. Only settings which are
     * acceptable on non-blocking connections are set. "direction" specifies whether the SSL_CTX
     * will be used to make outgoing connections or accept incoming connections.
     */
    virtual Status initSSLContext(SSL_CTX* context,
                                  const SSLParams& params,
                                  ConnectionDirection direction) = 0;

    /**
     * Fetches a peer certificate and validates it if it exists. If validation fails, but weak
     * validation is enabled, boost::none will be returned. If validation fails, and invalid
     * certificates are not allowed, a non-OK status will be returned. If validation is successful,
     * an engaged optional containing the certificate's subject name will be returned.
     */
    virtual StatusWith<boost::optional<std::string>> parseAndValidatePeerCertificate(
        SSL* ssl, const std::string& remoteHost) = 0;
};

// Access SSL functions through this instance.
SSLManagerInterface* getSSLManager();

extern bool isSSLServer;

/**
 * The global SSL configuration. This should be accessed only after global initialization has
 * completed. If it must be accessed in an initializer, the initializer should have
 * "EndStartupOptionStorage" as a prerequisite.
 */
const SSLParams& getSSLGlobalParams();
}
#endif  // #ifdef MONGO_CONFIG_SSL
