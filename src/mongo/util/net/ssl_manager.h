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

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace mongo {
    class SSLManager {
    MONGO_DISALLOW_COPYING(SSLManager);
    public:
        SSLManager();

        /** @return true if was successful, otherwise false */
        bool setupPEM( const std::string& keyFile , const std::string& password );

        /*
         * Set up SSL for certificate validation by loading a CA
         */
        bool setupCA(const std::string& caFile);

        /**
         * Initiates a TLS connection.
         * Throws SocketException on failure.
         * @return a pointer to an SSL context; caller must SSL_free it.
         */
        SSL* connect(int fd);

        /**
         * Waits for the other side to initiate a TLS connection.
         * Throws SocketException on failure.
         * @return a pointer to an SSL context; caller must SSL_free it.
         */
        SSL* accept(int fd);

        /**
         * Fetches a peer certificate and validates it if it exists
         * Throws SocketException on failure
         */
        void validatePeerCertificate(const SSL* ssl);

        /**
         * Callbacks for SSL functions
         */
        static int password_cb( char *buf,int num, int rwflag,void *userdata );
        static int verify_cb(int ok, X509_STORE_CTX *ctx);

        static SSLManager* getGlobal();
        static SSLManager* createGlobal();

    private:
        SSL_CTX* _context;
        std::string _password;
        bool _validateCertificates;

        /**
         * creates an SSL context to be used for this file descriptor.
         * caller must SSL_free it.
         */
        SSL* _secure(int fd);

        /**
         * Fetches the error text for an error code, in a thread-safe manner.
         */
        std::string _getSSLErrorMessage(int code);

        /**
         * Given an error code from an SSL-type IO function, logs an 
         * appropriate message and throws a SocketException
         */
        void _handleSSLError(int code);
    };
}
#endif
