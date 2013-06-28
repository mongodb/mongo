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

#include "mongo/pch.h"

#include "mongo/util/net/ssl_manager.h"

#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/tss.hpp>
#include <string>
#include <vector>

#include "mongo/bson/util/atomic_int.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

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

    static unsigned long _ssl_id_callback();
    static void _ssl_locking_callback(int mode, int type, const char *file, int line);

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

    static unsigned long _ssl_id_callback() {
        return SSLThreadInfo::get()->id();
    }
    static void _ssl_locking_callback(int mode, int type, const char *file, int line) {
        SSLThreadInfo::get()->lock_callback( mode , type , file , line );
    }

    AtomicUInt SSLThreadInfo::_next;
    std::vector<boost::recursive_mutex*> SSLThreadInfo::_mutex;
    boost::thread_specific_ptr<SSLThreadInfo> SSLThreadInfo::_thread;
    
    ////////////////////////////////////////////////////////////////

    static mongo::mutex sslInitMtx("SSL Initialization");
    static bool sslInitialized(false);

    void SSLManager::_initializeSSL(const SSLParams& params) {
        scoped_lock lk(sslInitMtx);
        if (sslInitialized) 
            return;  // already done

        SSL_library_init();
        SSL_load_error_strings();
        ERR_load_crypto_strings();

        if (params.fipsMode) {
            _setupFIPS();
        }

        // Add all digests and ciphers to OpenSSL's internal table
        // so that encryption/decryption is backwards compatible
        OpenSSL_add_all_algorithms();

        sslInitialized = true;
    }

    SSLManager::SSLManager(const SSLParams& params) : 
        _validateCertificates(false),
        _weakValidation(params.weakCertificateValidation) {
        
        _initializeSSL(params);
  
        _context = SSL_CTX_new(SSLv23_method());
        massert(15864, 
                mongoutils::str::stream() << "can't create SSL Context: " << 
                _getSSLErrorMessage(ERR_get_error()), 
                _context);
   
        // Activate all bug workaround options, to support buggy client SSL's.
        SSL_CTX_set_options(_context, SSL_OP_ALL);

        // If renegotiation is needed, don't return from recv() or send() until it's successful.
        // Note: this is for blocking sockets only.
        SSL_CTX_set_mode(_context, SSL_MODE_AUTO_RETRY);

        // Set context within which session can be reused
        int status = SSL_CTX_set_session_id_context(
            _context,
            static_cast<unsigned char*>(static_cast<void*>(&_context)),
            sizeof(_context));
        if (!status) {
            uasserted(16768,"ssl initialization problem");
        }

        SSLThreadInfo::init();
        SSLThreadInfo::get();

        if (!params.pemfile.empty()) {
            if (!_setupPEM(params.pemfile, params.pempwd)) {
                uasserted(16562, "ssl initialization problem"); 
            }
        }
        if (!params.cafile.empty()) {
            // Set up certificate validation with a certificate authority
            if (!_setupCA(params.cafile)) {
                uasserted(16563, "ssl initialization problem"); 
            }
        }
        if (!params.crlfile.empty()) {
            if (!_setupCRL(params.crlfile)) {
                uasserted(16582, "ssl initialization problem");
            }
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

    void SSLManager::_setupFIPS() {
        // Turn on FIPS mode if requested.
        int status = FIPS_mode_set(1);
        if (!status) {
            error() << "can't activate FIPS mode: " << 
                _getSSLErrorMessage(ERR_get_error()) << endl;
            fassertFailed(16703);
        }
        log() << "FIPS 140-2 mode activated" << endl;
    }

    bool SSLManager::_setupPEM(const std::string& keyFile , const std::string& password) {
        _password = password;
        
        if ( SSL_CTX_use_certificate_chain_file( _context , keyFile.c_str() ) != 1 ) {
            error() << "cannot read certificate file: " << keyFile << ' ' <<
                _getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        
        SSL_CTX_set_default_passwd_cb_userdata( _context , this );
        SSL_CTX_set_default_passwd_cb( _context, &SSLManager::password_cb );
        
        if ( SSL_CTX_use_PrivateKey_file( _context , keyFile.c_str() , SSL_FILETYPE_PEM ) != 1 ) {
            error() << "cannot read key file: " << keyFile << ' ' <<
                _getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        
        // Verify that the certificate and the key go together.
        if (SSL_CTX_check_private_key(_context) != 1) {
            error() << "SSL certificate validation: " << _getSSLErrorMessage(ERR_get_error()) 
                    << endl;
            return false;
        }
        return true;
    }

    bool SSLManager::_setupCA(const std::string& caFile) {
        // Load trusted CA
        if (SSL_CTX_load_verify_locations(_context, caFile.c_str(), NULL) != 1) {
            error() << "cannot read certificate authority file: " << caFile << " " <<
                _getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        // Set SSL to require peer (client) certificate verification
        // if a certificate is presented
        SSL_CTX_set_verify(_context, SSL_VERIFY_PEER, &SSLManager::verify_cb);
        _validateCertificates = true;
        return true;
    }

    bool SSLManager::_setupCRL(const std::string& crlFile) {
        X509_STORE *store = SSL_CTX_get_cert_store(_context);
        fassert(16583, store);
        
        X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK);
        X509_LOOKUP *lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
        fassert(16584, lookup);

        int status = X509_load_crl_file(lookup, crlFile.c_str(), X509_FILETYPE_PEM);
        if (status == 0) {
            error() << "cannot read CRL file: " << crlFile << ' ' <<
                _getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        log() << "ssl imported " << status << " revoked certificate" << 
            ((status == 1) ? "" : "s") << " from the revocation list." << 
            endl;
        return true;
    }
                
    SSL* SSLManager::_secure(int fd) {
        // This just ensures that SSL multithreading support is set up for this thread,
        // if it's not already.
        SSLThreadInfo::get();

        SSL * ssl = SSL_new(_context);
        massert(15861,
                _getSSLErrorMessage(ERR_get_error()),
                ssl);
        
        int status = SSL_set_fd( ssl , fd );
        massert(16510,
                _getSSLErrorMessage(ERR_get_error()), 
                status == 1);

        return ssl;
    }

    int SSLManager::_ssl_connect(SSL* ssl) {
        int ret = 0;
        for (int i=0; i<3; ++i) {
            ret = SSL_connect(ssl);
            if (ret == 1) 
                return ret;
            int code = SSL_get_error(ssl, ret);
            // Call SSL_connect again if we get SSL_ERROR_WANT_READ;
            // otherwise return error to caller.
            if (code != SSL_ERROR_WANT_READ)
                return ret;
        }
        // Give up and return connection-failure error to user
        return ret;
    }
    SSL* SSLManager::connect(int fd) {
        SSL* ssl = _secure(fd);
        ScopeGuard guard = MakeGuard(::SSL_free, ssl);
        int ret = _ssl_connect(ssl);
        if (ret != 1)
            _handleSSLError(SSL_get_error(ssl, ret));
        guard.Dismiss();
        return ssl;
    }

    SSL* SSLManager::accept(int fd) {
        SSL* ssl = _secure(fd);
        ScopeGuard guard = MakeGuard(::SSL_free, ssl);
        int ret = SSL_accept(ssl);
        if (ret != 1)
            _handleSSLError(SSL_get_error(ssl, ret));
        guard.Dismiss();
        return ssl;
    }

    void SSLManager::validatePeerCertificate(const SSL* ssl) {
        if (!_validateCertificates) return;

        X509* cert = SSL_get_peer_certificate(ssl);

        if (cert == NULL) { // no certificate presented by peer
            if (_weakValidation) {
                warning() << "no SSL certificate provided by peer" << endl;
            }
            else {
                error() << "no SSL certificate provided by peer; connection rejected" << endl;
                throw SocketException(SocketException::CONNECT_ERROR, "");
            }
            return;
        }
        ON_BLOCK_EXIT(X509_free, cert);

        long result = SSL_get_verify_result(ssl);

        if (result != X509_V_OK) {
            error() << "SSL peer certificate validation failed:" << 
                X509_verify_cert_error_string(result) << endl;
            throw SocketException(SocketException::CONNECT_ERROR, "");
        }

        // TODO: check optional cipher restriction, using cert.
    }

    void SSLManager::cleanupThreadLocals() {
        ERR_remove_state(0);
    }

    std::string SSLManager::_getSSLErrorMessage(int code) {
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
            error() << _getSSLErrorMessage(ret) << endl;
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
}

#endif
        
