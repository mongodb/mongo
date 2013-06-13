 /*
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

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace mongo {
    class SSLManagerInterface {
    public:
        virtual ~SSLManagerInterface();

        /**
         * Initiates a TLS connection.
         * Throws SocketException on failure.
         * @return a pointer to an SSL context; caller must SSL_free it.
         */
        virtual SSL* connect(int fd) = 0;

        /**
         * Waits for the other side to initiate a TLS connection.
         * Throws SocketException on failure.
         * @return a pointer to an SSL context; caller must SSL_free it.
         */
        virtual SSL* accept(int fd) = 0;

        /**
         * Fetches a peer certificate and validates it if it exists
         * Throws SocketException on failure
         * @return a std::string containing the certificate's subject name.
         */
        virtual std::string validatePeerCertificate(const SSL* ssl) = 0;

        /**
         * Cleans up SSL thread local memory; use at thread exit
         * to avoid memory leaks
         */
        virtual void cleanupThreadLocals() = 0;

        /**
         * Get the subject name of our own server certificate
         * @return the subject name.
         */
        virtual std::string getSubjectName() = 0;

        /**
         * ssl.h shims
         */
        virtual int SSL_read(SSL* ssl, void* buf, int num) = 0;

        virtual int SSL_write(SSL* ssl, const void* buf, int num) = 0;

        virtual unsigned long ERR_get_error() = 0;

        virtual char* ERR_error_string(unsigned long e, char* buf) = 0;

        virtual int SSL_get_error(const SSL* ssl, int ret) = 0;

        virtual int SSL_shutdown(SSL* ssl) = 0;

        virtual void SSL_free(SSL* ssl) = 0;
    };

    // Access SSL functions through this instance.
    SSLManagerInterface* getSSLManager();

}
#endif
