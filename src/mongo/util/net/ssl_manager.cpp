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

#include "mongo/pch.h"

#include "mongo/util/net/ssl_manager.h"

#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/tss.hpp>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/util/atomic_int.h"
#include "mongo/db/cmdline.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"

#ifdef MONGO_SSL
#include <openssl/evp.h>
#endif

namespace mongo {
#ifndef MONGO_SSL   
    const std::string getSSLVersion(const std::string &prefix, const std::string &suffix) {
        return "";
    }
#else
    const std::string getSSLVersion(const std::string &prefix, const std::string &suffix) {
        return prefix + SSLeay_version(SSLEAY_VERSION) + suffix;
    }

    namespace {

        /**
         * Multithreaded Support for SSL.
         *
         * In order to allow OpenSSL to work in a multithreaded environment, you
         * must provide some callbacks for it to use for locking.  The following code
         * sets up a vector of mutexes and uses thread-local storage to assign an id
         * to each thread.
         * The so-called SSLThreadInfo class encapsulates most of the logic required for
         * OpenSSL multithreaded support.
         */

        unsigned long _ssl_id_callback();
        void _ssl_locking_callback(int mode, int type, const char *file, int line);

        class SSLThreadInfo {
        public:

            SSLThreadInfo() {
                _id = ++_next;
                CRYPTO_set_id_callback(_ssl_id_callback);
                CRYPTO_set_locking_callback(_ssl_locking_callback);
            }

            ~SSLThreadInfo() {
                CRYPTO_set_id_callback(0);
            }

            unsigned long id() const { return _id; }

            void lock_callback( int mode, int type, const char *file, int line ) {
                if ( mode & CRYPTO_LOCK ) {
                    _mutex[type]->lock();
                }
                else {
                    _mutex[type]->unlock();
                }
            }

            static void init() {
                while ( (int)_mutex.size() < CRYPTO_num_locks() )
                    _mutex.push_back( new boost::recursive_mutex );
            }

            static SSLThreadInfo* get() {
                SSLThreadInfo* me = _thread.get();
                if ( ! me ) {
                    me = new SSLThreadInfo();
                    _thread.reset( me );
                }
                return me;
            }

        private:
            unsigned _id;

            static AtomicUInt _next;
            // Note: see SERVER-8734 for why we are using a recursive mutex here.
            // Once the deadlock fix in OpenSSL is incorporated into most distros of
            // Linux, this can be changed back to a nonrecursive mutex.
            static std::vector<boost::recursive_mutex*> _mutex;
            static boost::thread_specific_ptr<SSLThreadInfo> _thread;
        };

        unsigned long _ssl_id_callback() {
            return SSLThreadInfo::get()->id();
        }

        void _ssl_locking_callback(int mode, int type, const char *file, int line) {
            SSLThreadInfo::get()->lock_callback( mode , type , file , line );
        }

        AtomicUInt SSLThreadInfo::_next;
        std::vector<boost::recursive_mutex*> SSLThreadInfo::_mutex;
        boost::thread_specific_ptr<SSLThreadInfo> SSLThreadInfo::_thread;

        ////////////////////////////////////////////////////////////////

        SimpleMutex sslManagerMtx("SSL Manager");
        SSLManagerInterface* theSSLManager = NULL;
        static const int BUFFER_SIZE = 8*1024;

        struct Params {
            Params(const std::string& pemfile,
                   const std::string& pempwd,
                   const std::string& clusterfile,
                   const std::string& clusterpwd,
                   const std::string& cafile = "",
                   const std::string& crlfile = "",
                   bool weakCertificateValidation = false,
                   bool fipsMode = false) :
                pemfile(pemfile),
                pempwd(pempwd),
                clusterfile(clusterfile),
                clusterpwd(clusterpwd),
                cafile(cafile),
                crlfile(crlfile),
                weakCertificateValidation(weakCertificateValidation),
                fipsMode(fipsMode) {};

            std::string pemfile;
            std::string pempwd;
            std::string clusterfile;
            std::string clusterpwd;
            std::string cafile;
            std::string crlfile;
            bool weakCertificateValidation;
            bool fipsMode;
        };

        class SSLManager : public SSLManagerInterface {
        public:
            explicit SSLManager(const Params& params, bool isServer);

            virtual ~SSLManager();

            virtual SSLConnection* connect(Socket* socket);

            virtual SSLConnection* accept(Socket* socket);

            virtual std::string validatePeerCertificate(const SSLConnection* conn);

            virtual void cleanupThreadLocals();

            virtual std::string getServerSubjectName() {
                return _serverSubjectName;
            }

            virtual std::string getClientSubjectName() {
                return _clientSubjectName;
            }

            virtual std::string getSSLErrorMessage(int code);

            virtual int SSL_read(SSLConnection* conn, void* buf, int num);

            virtual int SSL_write(SSLConnection* conn, const void* buf, int num);

            virtual unsigned long ERR_get_error();

            virtual char* ERR_error_string(unsigned long e, char* buf);

            virtual int SSL_get_error(const SSLConnection* conn, int ret);

            virtual int SSL_shutdown(SSLConnection* conn);

            virtual void SSL_free(SSLConnection* conn);

        private:
            SSL_CTX* _serverContext;  // SSL context for incoming connections
            SSL_CTX* _clientContext;  // SSL context for outgoing connections
            std::string _password;
            bool _validateCertificates;
            bool _weakValidation;
            std::string _serverSubjectName;
            std::string _clientSubjectName;

            /**
             * creates an SSL object to be used for this file descriptor.
             * caller must SSL_free it.
             */
            SSL* _secure(SSL_CTX* context, int fd);

            /**
             * Given an error code from an SSL-type IO function, logs an
             * appropriate message and throws a SocketException
             */
            MONGO_COMPILER_NORETURN void _handleSSLError(int code);

            /*
             * Init the SSL context using parameters provided in params.
             */
            bool _initSSLContext(SSL_CTX** context, const Params& params);

            /*
             * Parse the x509 subject name from the PEM keyfile and store it 
             */
            bool _setSubjectName(const std::string& keyFile, std::string& subjectName);

            /** @return true if was successful, otherwise false */
            bool _setupPEM(SSL_CTX* context,
                           const std::string& keyFile,
                           const std::string& password);

            /*
             * Set up an SSL context for certificate validation by loading a CA
             */
            bool _setupCA(SSL_CTX* context, const std::string& caFile);

            /*
             * Import a certificate revocation list into an SSL context
             * for use with validating certificates
             */
            bool _setupCRL(SSL_CTX* context, const std::string& crlFile);

            /*
             * Activate FIPS 140-2 mode, if the server started with a command line
             * parameter.
             */
            void _setupFIPS();

            /*
             * sub function for checking the result of an SSL operation
             */
            bool _doneWithSSLOp(SSLConnection* conn, int status);

            /*
             * Send and receive network data
             */
            void _flushNetworkBIO(SSLConnection* conn);

            /**
             * Callbacks for SSL functions
             */
            static int password_cb( char *buf,int num, int rwflag,void *userdata );
            static int verify_cb(int ok, X509_STORE_CTX *ctx);

        };

    } // namespace

    // Global variable indicating if this is a server or a client instance
    bool isSSLServer = false;
    
    MONGO_INITIALIZER(SSLManager)(InitializerContext* context) {
        SimpleMutex::scoped_lock lck(sslManagerMtx);
        if (cmdLine.sslOnNormalPorts) {
            const Params params(
                cmdLine.sslPEMKeyFile,
                cmdLine.sslPEMKeyPassword,
                cmdLine.sslClusterFile,
                cmdLine.sslClusterPassword,
                cmdLine.sslCAFile,
                cmdLine.sslCRLFile,
                cmdLine.sslWeakCertificateValidation,
                cmdLine.sslFIPSMode);
            theSSLManager = new SSLManager(params, isSSLServer);
        }
        return Status::OK();
    }

    SSLManagerInterface* getSSLManager() {
        SimpleMutex::scoped_lock lck(sslManagerMtx);
        if (theSSLManager)
            return theSSLManager;
        return NULL;
    }

    std::string getCertificateSubjectName(X509* cert) {
        std::string result;

        BIO* out = BIO_new(BIO_s_mem());
        uassert(16884, "unable to allocate BIO memory", NULL != out);
        ON_BLOCK_EXIT(BIO_free, out);

        if (X509_NAME_print_ex(out,
                              X509_get_subject_name(cert),
                              0,
                              XN_FLAG_RFC2253) >= 0) {
            if (BIO_number_written(out) > 0) {
                result.resize(BIO_number_written(out));
                BIO_read(out, &result[0], result.size());
            }
        }
        else {
            log() << "failed to convert subject name to RFC2253 format" << endl;
        }

        return result;
    }

    SSLConnection::SSLConnection(SSL_CTX* context, Socket* sock) : socket(sock) {
        // This just ensures that SSL multithreading support is set up for this thread,
        // if it's not already.
        SSLThreadInfo::get();

        ssl = SSL_new(context);
 
        std::string sslErr = NULL != getSSLManager() ? 
            getSSLManager()->getSSLErrorMessage(ERR_get_error()) : "";
        massert(15861, "Error creating new SSL object " + sslErr, ssl);

        BIO_new_bio_pair(&internalBIO, BUFFER_SIZE, &networkBIO, BUFFER_SIZE);
        SSL_set_bio(ssl, internalBIO, internalBIO);
    }

    SSLConnection::~SSLConnection() {
        if (ssl) {   // The internalBIO is automatically freed as part of SSL_free
            SSL_free(ssl);
        }
        if (networkBIO) {
            BIO_free(networkBIO);
        }
    }

    SSLManagerInterface::~SSLManagerInterface() {}

    SSLManager::SSLManager(const Params& params, bool isServer) :
        _validateCertificates(false),
        _weakValidation(params.weakCertificateValidation) {

        SSL_library_init();
        SSL_load_error_strings();
        ERR_load_crypto_strings();

        if (params.fipsMode) {
            _setupFIPS();
        }

        // Add all digests and ciphers to OpenSSL's internal table
        // so that encryption/decryption is backwards compatible
        OpenSSL_add_all_algorithms();
 
        SSLThreadInfo::init();
        SSLThreadInfo::get();

        if (!_initSSLContext(&_clientContext, params)) {
            uasserted(16768, "ssl initialization problem"); 
        }

        // SSL client specific initialization
        if (!isServer) {
            _serverContext = NULL;

            if (!params.pemfile.empty()) {
                if (!_setSubjectName(params.pemfile, _clientSubjectName)) {
                    uasserted(16941, "ssl initialization problem"); 
                }
            }
        }
        // SSL server specific initialization
        if (isServer) {
            if (!_initSSLContext(&_serverContext, params)) {
                uasserted(16562, "ssl initialization problem"); 
            }

            if (!_setSubjectName(params.pemfile, _serverSubjectName)) {
                uasserted(16942, "ssl initialization problem"); 
            }
            // use the cluster certificate for outgoing connections if specified
            if (!params.clusterfile.empty()) {
                if (!_setSubjectName(params.clusterfile, _clientSubjectName)) {
                    uasserted(16943, "ssl initialization problem"); 
                }
            }
            else { 
                if (!_setSubjectName(params.pemfile, _clientSubjectName)) {
                    uasserted(16944, "ssl initialization problem"); 
                }
            }
        }
    }

    SSLManager::~SSLManager() {
        ERR_free_strings();
        EVP_cleanup();

        if (NULL != _serverContext) {
            SSL_CTX_free(_serverContext);
        }
        if (NULL != _clientContext) {
            SSL_CTX_free(_clientContext);
        }
    }

    int SSLManager::password_cb(char *buf,int num, int rwflag,void *userdata) {
        SSLManager* sm = static_cast<SSLManager*>(userdata);
        std::string pass = sm->_password;
        strcpy(buf,pass.c_str());
        return(pass.size());
    }

    int SSLManager::verify_cb(int ok, X509_STORE_CTX *ctx) {
	return 1; // always succeed; we will catch the error in our get_verify_result() call
    }

    int SSLManager::SSL_read(SSLConnection* conn, void* buf, int num) {
        int status;
        do {
            status = ::SSL_read(conn->ssl, buf, num);
        } while(!_doneWithSSLOp(conn, status)); 
 
        if (status <= 0)
            _handleSSLError(SSL_get_error(conn, status));
        return status;
    }

    int SSLManager::SSL_write(SSLConnection* conn, const void* buf, int num) {
        int status;
        do {
            status = ::SSL_write(conn->ssl, buf, num);
        } while(!_doneWithSSLOp(conn, status));
 
        if (status <= 0)
            _handleSSLError(SSL_get_error(conn, status));
        return status;
    }

    unsigned long SSLManager::ERR_get_error() {
        return ::ERR_get_error();
    }

    char* SSLManager::ERR_error_string(unsigned long e, char* buf) {
        return ::ERR_error_string(e, buf);
    }

    int SSLManager::SSL_get_error(const SSLConnection* conn, int ret) {
        return ::SSL_get_error(conn->ssl, ret);
    }

    int SSLManager::SSL_shutdown(SSLConnection* conn) {
        int status;
        do {
            status = ::SSL_shutdown(conn->ssl);
        } while(!_doneWithSSLOp(conn, status));
 
        if (status < 0)
            _handleSSLError(SSL_get_error(conn, status));
        return status;
    }

    void SSLManager::SSL_free(SSLConnection* conn) {
        return ::SSL_free(conn->ssl);
    }

    void SSLManager::_setupFIPS() {
        // Turn on FIPS mode if requested.
#ifdef OPENSSL_FIPS
        int status = FIPS_mode_set(1);
        if (!status) {
            error() << "can't activate FIPS mode: " << 
                getSSLErrorMessage(ERR_get_error()) << endl;
            fassertFailed(16703);
        }
        log() << "FIPS 140-2 mode activated" << endl;
#else
        error() << "this version of mongodb was not compiled with FIPS support";
        fassertFailed(17089);
#endif
    }

    bool SSLManager::_initSSLContext(SSL_CTX** context, const Params& params) {
        *context = SSL_CTX_new(SSLv23_method());
        massert(15864,
                mongoutils::str::stream() << "can't create SSL Context: " <<
                getSSLErrorMessage(ERR_get_error()),
                context);

        // SSL_OP_ALL - Activate all bug workaround options, to support buggy client SSL's.
        // SSL_OP_NO_SSLv2 - Disable SSL v2 support 
        SSL_CTX_set_options(*context, SSL_OP_ALL|SSL_OP_NO_SSLv2);

        // If renegotiation is needed, don't return from recv() or send() until it's successful.
        // Note: this is for blocking sockets only.
        SSL_CTX_set_mode(*context, SSL_MODE_AUTO_RETRY);

        // Set context within which session can be reused
        int status = SSL_CTX_set_session_id_context(
            *context,
            static_cast<unsigned char*>(static_cast<void*>(context)),
            sizeof(*context));
 
        if (!status) {
            error() << "failed to set session id context: " << 
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }

        // Use the clusterfile for internal outgoing SSL connections if specified 
        if (context == &_clientContext && !params.clusterfile.empty()) {
            EVP_set_pw_prompt("Enter cluster certificate passphrase");
            if (!_setupPEM(*context, params.clusterfile, params.clusterpwd)) {
                return false;
            }
        }
        // Use the pemfile for everything else
        else if (!params.pemfile.empty()) {
            EVP_set_pw_prompt("Enter PEM passphrase");
            if (!_setupPEM(*context, params.pemfile, params.pempwd)) {
                return false;
            }
        }

        if (!params.cafile.empty()) {
            // Set up certificate validation with a certificate authority
            if (!_setupCA(*context, params.cafile)) {
                return false;
            }
        }

        if (!params.crlfile.empty()) {
            if (!_setupCRL(*context, params.crlfile)) {
                return false;
            }
        }

        return true;
    }

    bool SSLManager::_setSubjectName(const std::string& keyFile, std::string& subjectName) {
        // Read the certificate subject name and store it 
        BIO *in = BIO_new(BIO_s_file_internal());
        if (NULL == in){
            error() << "failed to allocate BIO object: " << 
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        ON_BLOCK_EXIT(BIO_free, in);

        if (BIO_read_filename(in, keyFile.c_str()) <= 0){
            error() << "cannot read key file when setting subject name: " << keyFile << ' ' <<
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }

        X509* x509 = PEM_read_bio_X509(in, NULL, &SSLManager::password_cb, this);
        if (NULL == x509) {
            error() << "cannot retreive certificate from keyfile: " << keyFile << ' ' <<
                getSSLErrorMessage(ERR_get_error()) << endl; 
            return false;
        }
        ON_BLOCK_EXIT(X509_free, x509);
        subjectName = getCertificateSubjectName(x509);

        return true;
    }

    bool SSLManager::_setupPEM(SSL_CTX* context, 
                               const std::string& keyFile, 
                               const std::string& password) {
        _password = password;
 
        if ( SSL_CTX_use_certificate_chain_file( context , keyFile.c_str() ) != 1 ) {
            error() << "cannot read certificate file: " << keyFile << ' ' <<
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }

        // If password is empty, use default OpenSSL callback, which uses the terminal
        // to securely request the password interactively from the user.
        if (!password.empty()) {
            SSL_CTX_set_default_passwd_cb_userdata( context , this );
            SSL_CTX_set_default_passwd_cb( context, &SSLManager::password_cb );
        }
 
        if ( SSL_CTX_use_PrivateKey_file( context , keyFile.c_str() , SSL_FILETYPE_PEM ) != 1 ) {
            error() << "cannot read PEM key file: " << keyFile << ' ' <<
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
 
        // Verify that the certificate and the key go together.
        if (SSL_CTX_check_private_key(context) != 1) {
            error() << "SSL certificate validation: " << getSSLErrorMessage(ERR_get_error()) 
                    << endl;
            return false;
        }
 
        return true;
    }

    bool SSLManager::_setupCA(SSL_CTX* context, const std::string& caFile) {
        // Load trusted CA
        if (SSL_CTX_load_verify_locations(context, caFile.c_str(), NULL) != 1) {
            error() << "cannot read certificate authority file: " << caFile << " " <<
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        // Set SSL to require peer (client) certificate verification
        // if a certificate is presented
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, &SSLManager::verify_cb);
        _validateCertificates = true;
        return true;
    }

    bool SSLManager::_setupCRL(SSL_CTX* context, const std::string& crlFile) {
        X509_STORE *store = SSL_CTX_get_cert_store(context);
        fassert(16583, store);
        
        X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK);
        X509_LOOKUP *lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
        fassert(16584, lookup);

        int status = X509_load_crl_file(lookup, crlFile.c_str(), X509_FILETYPE_PEM);
        if (status == 0) {
            error() << "cannot read CRL file: " << crlFile << ' ' <<
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        log() << "ssl imported " << status << " revoked certificate" << 
            ((status == 1) ? "" : "s") << " from the revocation list." << 
            endl;
        return true;
    }

    /* 
    * The interface layer between network and BIO-pair. The BIO-pair buffers
    * the data to/from the TLS layer.
    */
    void SSLManager::_flushNetworkBIO(SSLConnection* conn){
        char buffer[BUFFER_SIZE];
        int wantWrite;

        /* 
        * Write the complete contents of the buffer. Leaving the buffer
        * unflushed could cause a deadlock. 
        */
        while ((wantWrite = BIO_ctrl_pending(conn->networkBIO)) > 0) {
            if (wantWrite > BUFFER_SIZE) {
                wantWrite = BUFFER_SIZE;
            }
            int fromBIO = BIO_read(conn->networkBIO, buffer, wantWrite);

            int writePos = 0;
            do {
                int numWrite = fromBIO - writePos;
                numWrite = send(conn->socket->rawFD(), buffer + writePos, numWrite, portSendFlags); 
                if (numWrite < 0) {
                    conn->socket->handleSendError(numWrite, "");
                }
                writePos += numWrite;
            } while (writePos < fromBIO);
        }

        int wantRead;
        while ((wantRead = BIO_ctrl_get_read_request(conn->networkBIO)) > 0)
        {
            if (wantRead > BUFFER_SIZE) {
                wantRead = BUFFER_SIZE;
            }

            int numRead = recv(conn->socket->rawFD(), buffer, wantRead, portRecvFlags);
            if (numRead <= 0) {
                conn->socket->handleRecvError(numRead, wantRead);
                continue;
            }

            int toBIO = BIO_write(conn->networkBIO, buffer, numRead);
            if (toBIO != numRead) {
                LOG(3) << "Failed to write network data to the SSL BIO layer";
                throw SocketException(SocketException::RECV_ERROR , conn->socket->remoteString());
            }
        } 
    }

    bool SSLManager::_doneWithSSLOp(SSLConnection* conn, int status) {
        int sslErr = SSL_get_error(conn, status);
        switch (sslErr) {
            case SSL_ERROR_NONE:
                _flushNetworkBIO(conn);     // success, flush network BIO before leaving
                return true;
            case SSL_ERROR_WANT_WRITE:
            case SSL_ERROR_WANT_READ:
                _flushNetworkBIO(conn);     // not ready, flush network BIO and try again
                return false;
            default:
                return true;
        }
    }

    SSLConnection* SSLManager::connect(Socket* socket) {
        SSLConnection* sslConn = new SSLConnection(_clientContext, socket);
        ScopeGuard sslGuard = MakeGuard(::SSL_free, sslConn->ssl);
        ScopeGuard bioGuard = MakeGuard(::BIO_free, sslConn->networkBIO);
 
        int ret;
        do {
            ret = ::SSL_connect(sslConn->ssl);
        } while(!_doneWithSSLOp(sslConn, ret));
 
        if (ret != 1)
            _handleSSLError(SSL_get_error(sslConn, ret));
 
        sslGuard.Dismiss();
        bioGuard.Dismiss();
        return sslConn;
    }

    SSLConnection* SSLManager::accept(Socket* socket) {
        SSLConnection* sslConn = new SSLConnection(_serverContext, socket);
        ScopeGuard sslGuard = MakeGuard(::SSL_free, sslConn->ssl);
        ScopeGuard bioGuard = MakeGuard(::BIO_free, sslConn->networkBIO);
 
        int ret;
        do {
            ret = ::SSL_accept(sslConn->ssl);
        } while(!_doneWithSSLOp(sslConn, ret));
 
        if (ret != 1)
            _handleSSLError(SSL_get_error(sslConn, ret));
 
        sslGuard.Dismiss();
        bioGuard.Dismiss();
        return sslConn;
    }

    std::string SSLManager::validatePeerCertificate(const SSLConnection* conn) {
        if (!_validateCertificates) return "";

        X509* peerCert = SSL_get_peer_certificate(conn->ssl);

        if (NULL == peerCert) { // no certificate presented by peer
            if (_weakValidation) {
                warning() << "no SSL certificate provided by peer" << endl;
            }
            else {
                error() << "no SSL certificate provided by peer; connection rejected" << endl;
                throw SocketException(SocketException::CONNECT_ERROR, "");
            }
            return "";
        }
        ON_BLOCK_EXIT(X509_free, peerCert);

        long result = SSL_get_verify_result(conn->ssl);

        if (result != X509_V_OK) {
            error() << "SSL peer certificate validation failed:" << 
                X509_verify_cert_error_string(result) << endl;
            throw SocketException(SocketException::CONNECT_ERROR, "");
        }
 
        // TODO: check optional cipher restriction, using cert.
        return getCertificateSubjectName(peerCert);
    }

    void SSLManager::cleanupThreadLocals() {
        ERR_remove_state(0);
    }

    std::string SSLManager::getSSLErrorMessage(int code) {
        // 120 from the SSL documentation for ERR_error_string
        static const size_t msglen = 120;

        char msg[msglen];
        ERR_error_string_n(code, msg, msglen);
        return msg;
    }

    void SSLManager::_handleSSLError(int code) {
        switch (code) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // should not happen because we turned on AUTO_RETRY
            // However, it turns out this CAN happen during a connect, if the other side
            // accepts the socket connection but fails to do the SSL handshake in a timely
            // manner.
            error() << "SSL error: " << code << ", possibly timed out during connect" << endl;
            break;

        case SSL_ERROR_SYSCALL:
            if (code < 0) {
                error() << "socket error: " << errnoWithDescription() << endl;
            }
            else {
                error() << "could not negotiate SSL connection: EOF detected" << endl;
            }
            break;

        case SSL_ERROR_SSL:
        {
            int ret = ERR_get_error();
            error() << getSSLErrorMessage(ret) << endl;
            break;
        }
        case SSL_ERROR_ZERO_RETURN:
            error() << "could not negotiate SSL connection: EOF detected" << endl;
            break;
        
        default:
            error() << "unrecognized SSL error" << endl;
            break;
        }
        throw SocketException(SocketException::CONNECT_ERROR, "");
    }
#endif // #ifdef MONGO_SSL
}
