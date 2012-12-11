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

#include <vector>
#include <string>
#include <boost/thread/tss.hpp>
#include "mongo/bson/util/atomic_int.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    static const char* validateErrToString(long code);

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
                _mutex.push_back( new SimpleMutex("SSLThreadInfo") );
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
        static std::vector<SimpleMutex*> _mutex;
        static boost::thread_specific_ptr<SSLThreadInfo> _thread;
    };

    static unsigned long _ssl_id_callback() {
        return SSLThreadInfo::get()->id();
    }
    static void _ssl_locking_callback(int mode, int type, const char *file, int line) {
        SSLThreadInfo::get()->lock_callback( mode , type , file , line );
    }

    AtomicUInt SSLThreadInfo::_next;
    std::vector<SimpleMutex*> SSLThreadInfo::_mutex;
    boost::thread_specific_ptr<SSLThreadInfo> SSLThreadInfo::_thread;
    
    ////////////////////////////////////////////////////////////////

    SSLManager::SSLManager(std::string pemfile,
                           std::string pempwd,
                           std::string cafile) : 
        _validateCertificates(false) {
        SSL_library_init();
        SSL_load_error_strings();
        ERR_load_crypto_strings();
        
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

        SSLThreadInfo::init();
        SSLThreadInfo::get();

        if (!pemfile.empty()) {
            if (!_setupPEM(pemfile, pempwd)) {
                uasserted(16562, "ssl initialization problem"); 
            }
        }
        if (!cafile.empty()) {
            // Set up certificate validation with a certificate authority
            if (!_setupCA(cafile)) {
                uasserted(16563, "ssl initialization problem"); 
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

    bool SSLManager::_setupPEM(const std::string& keyFile , const std::string& password) {
        _password = password;
        
        if ( SSL_CTX_use_certificate_chain_file( _context , keyFile.c_str() ) != 1 ) {
            log() << "Can't read certificate file: " << keyFile << " " <<
                _getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        
        SSL_CTX_set_default_passwd_cb_userdata( _context , this );
        SSL_CTX_set_default_passwd_cb( _context, &SSLManager::password_cb );
        
        if ( SSL_CTX_use_PrivateKey_file( _context , keyFile.c_str() , SSL_FILETYPE_PEM ) != 1 ) {
            log() << "Can't read key file: " << keyFile << " " <<
                _getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        
        // Verify that the certificate and the key go together.
        if (SSL_CTX_check_private_key(_context) != 1) {
            log() << "SSL certificate validation: " << _getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        return true;
    }

    bool SSLManager::_setupCA(const std::string& caFile) {
        // Load trusted CA
        if (SSL_CTX_load_verify_locations(_context, caFile.c_str(), NULL) != 1) {
            log() << "Can't read certificate authority file: " << caFile << " " <<
                _getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        // Set SSL to require peer (client) certificate verification
        // if a certificate is presented
        SSL_CTX_set_verify(_context, SSL_VERIFY_PEER, &SSLManager::verify_cb);
        _validateCertificates = true;
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

    SSL* SSLManager::connect(int fd) {
        SSL* ssl = _secure(fd);
        int ret = SSL_connect(ssl);
        if (ret != 1)
            _handleSSLError(SSL_get_error(ssl, ret));
        return ssl;
    }

    SSL* SSLManager::accept(int fd) {
        SSL* ssl = _secure(fd);
        int ret = SSL_accept(ssl);
        if (ret != 1)
            _handleSSLError(SSL_get_error(ssl, ret));
        return ssl;
    }

    void SSLManager::validatePeerCertificate(const SSL* ssl) {
        if (!_validateCertificates) return;

        X509* cert = SSL_get_peer_certificate(ssl);

        if (cert == NULL) { // no certificate presented by peer
            // TODO: if force_validation, throw an exception
            // else
            log() << "no SSL certificate provided by peer" << endl;
            return;
        }
        ON_BLOCK_EXIT(X509_free, cert);

        long result = SSL_get_verify_result(ssl);

        if (result != X509_V_OK) {
            error() << "SSL peer certificate validation failed:" << validateErrToString(result)
                    << endl;
            throw SocketException(SocketException::CONNECT_ERROR, "");
        }

        // TODO: check optional cipher restriction, using cert.
        // TODO: enforce CRL?
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
            log() << "SSL error" << endl;
            throw SocketException(SocketException::CONNECT_ERROR, "");
            break;

        case SSL_ERROR_SYSCALL:
            if (code < 0) {
                log() << "socket error: " << errnoWithDescription() << endl;
                throw SocketException(SocketException::CONNECT_ERROR, "");
            }
            log() << "could not negotiate SSL connection: EOF detected" << endl;
            throw SocketException(SocketException::CONNECT_ERROR, "");
            break;

        case SSL_ERROR_SSL:
        {
            int ret = ERR_get_error();
            log() << _getSSLErrorMessage(ret) << endl;
            throw SocketException(SocketException::CONNECT_ERROR, "");
            break;
        }
        case SSL_ERROR_ZERO_RETURN:
            log() << "could not negotiate SSL connection: EOF detected" << endl;
            throw SocketException(SocketException::CONNECT_ERROR, "");
            break;
        
        default:
            log() << "unrecognized SSL error" << endl;
            throw SocketException(SocketException::CONNECT_ERROR, "");
            break;
        }
    }

    const char* validateErrToString(long code) {
        switch (code) {
        case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
            return "UNABLE_TO_DECRYPT_CERT_SIGNATURE";
        
        case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
            return "UNABLE_TO_DECRYPT_CRL_SIGNATURE";

        case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
            return "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY";

        case X509_V_ERR_CERT_SIGNATURE_FAILURE:
            return "CERT_SIGNATURE_FAILURE";

        case X509_V_ERR_CRL_SIGNATURE_FAILURE:
            return "CRL_SIGNATURE_FAILURE";

        case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
            return "ERROR_IN_CERT_NOT_BEFORE_FIELD";

        case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
            return "ERROR_IN_CERT_NOT_AFTER_FIELD";

        case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
            return "ERROR_IN_CRL_LAST_UPDATE_FIELD";

        case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
            return "ERROR_IN_CRL_NEXT_UPDATE_FIELD";

        case X509_V_ERR_CERT_NOT_YET_VALID:
            return "CERT_NOT_YET_VALID";

        case X509_V_ERR_CERT_HAS_EXPIRED:
            return "CERT_HAS_EXPIRED";

        case X509_V_ERR_OUT_OF_MEM:
            return "OUT_OF_MEM";

        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
            return "UNABLE_TO_GET_ISSUER_CERT";

        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
            return "UNABLE_TO_GET_ISSUER_CERT_LOCALLY";

        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
            return "UNABLE_TO_VERIFY_LEAF_SIGNATURE";

        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
            return "DEPTH_ZERO_SELF_SIGNED_CERT";

        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
            return "SELF_SIGNED_CERT_IN_CHAIN";

        case X509_V_ERR_CERT_CHAIN_TOO_LONG:
            return "CERT_CHAIN_TOO_LONG";

        case X509_V_ERR_CERT_REVOKED:
            return "CERT_REVOKED";

        case X509_V_ERR_INVALID_CA:
            return "INVALID_CA";

        case X509_V_ERR_PATH_LENGTH_EXCEEDED:
            return "PATH_LENGTH_EXCEEDED";

        case X509_V_ERR_INVALID_PURPOSE:
            return "INVALID_PURPOSE";

        case X509_V_ERR_CERT_UNTRUSTED:
            return "CERT_UNTRUSTED";

        case X509_V_ERR_CERT_REJECTED:
            return "CERT_REJECTED";

        case X509_V_ERR_UNABLE_TO_GET_CRL:
            return "UNABLE_TO_GET_CRL";

        case X509_V_ERR_CRL_NOT_YET_VALID:
            return "CRL_NOT_YET_VALID";

        case X509_V_ERR_CRL_HAS_EXPIRED:
            return "CRL_HAS_EXPIRED";
        }

        return "Unknown verify error";
    }
}

#endif
        
