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
    class SSLParams {
    public:
        SSLParams(const std::string& pemfile, 
                  const std::string& pempwd,
                  const std::string& cafile = "",
                  const std::string& crlfile = "",
                  bool weakCertificateValidation = false,
                  bool fipsMode = false) :
            pemfile(pemfile),
            pempwd(pempwd),
            cafile(cafile),
            crlfile(crlfile),
            weakCertificateValidation(weakCertificateValidation),
            fipsMode(fipsMode) {};

        std::string pemfile;
        std::string pempwd;
        std::string cafile;
        std::string crlfile;
        bool weakCertificateValidation;
        bool fipsMode;
    };

    class SSLManager {
    MONGO_DISALLOW_COPYING(SSLManager);
    public:
        explicit SSLManager(const SSLParams& params);

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
         * Cleans up SSL thread local memory; use at thread exit
         * to avoid memory leaks
         */
        static void cleanupThreadLocals();

        /**
         * Callbacks for SSL functions
         */
        static int password_cb( char *buf,int num, int rwflag,void *userdata );
        static int verify_cb(int ok, X509_STORE_CTX *ctx);

    private:
        SSL_CTX* _context;
        std::string _password;
        bool _validateCertificates;
        bool _weakValidation;
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

        /** @return true if was successful, otherwise false */
        bool _setupPEM( const std::string& keyFile , const std::string& password );

        /*
         * Set up SSL for certificate validation by loading a CA
         */
        bool _setupCA(const std::string& caFile);

        /*
         * Import a certificate revocation list into our SSL context
         * for use with validating certificates
         */
        bool _setupCRL(const std::string& crlFile);

        /*
         * Activate FIPS 140-2 mode, if the server started with a command line
         * parameter.
         */
        void _setupFIPS();

        /*
         * Wrapper for SSL_Connect() that handles SSL_ERROR_WANT_READ,
         * see SERVER-7940
         */
        int _ssl_connect(SSL* ssl);

        /*
         * Initialize the SSL Library.
         * This function can be called multiple times; it ensures it only
         * does the SSL initialization once per process.
         */
        void _initializeSSL(const SSLParams& params);
    };
}
#endif
