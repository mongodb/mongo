/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#ifdef MONGO_SSL

#pragma once

#include <string>
#include "mongo/base/disallow_copying.h"
#include "mongo/util/net/sock.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#endif

namespace mongo {
    /*
     * @return the SSL version string prefixed with prefix and suffixed with suffix
     */
    const std::string getSSLVersion(const std::string &prefix, const std::string &suffix); 
}

#ifdef MONGO_SSL
namespace mongo {

    class SSLConnection {
    public:
        SSL* ssl;
        BIO* networkBIO;
        BIO* internalBIO;
        Socket* socket;

        SSLConnection(SSL_CTX* ctx, Socket* sock); 

        ~SSLConnection();
    };

    class SSLManagerInterface {
    public:
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
        virtual SSLConnection* accept(Socket* socket) = 0;

        /**
         * Fetches a peer certificate and validates it if it exists
         * Throws SocketException on failure
         * @return a std::string containing the certificate's subject name.
         */
        virtual std::string validatePeerCertificate(const SSLConnection* conn) = 0;

        /**
         * Cleans up SSL thread local memory; use at thread exit
         * to avoid memory leaks
         */
        virtual void cleanupThreadLocals() = 0;

        /**
         * Gets the subject name of our own server certificate
         * @return the subject name.
         */
        virtual std::string getServerSubjectName() = 0;

        /**
         * Gets the subject name of our own client certificate
         * used for cluster authentiation
         * @return the subject name.
         */
        virtual std::string getClientSubjectName() = 0;

        /**
        * Fetches the error text for an error code, in a thread-safe manner.
        */
        virtual std::string getSSLErrorMessage(int code) = 0;
 
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
    };

    // Access SSL functions through this instance.
    SSLManagerInterface* getSSLManager();

    extern bool isSSLServer;
}
#endif // #ifdef MONGO_SSL
