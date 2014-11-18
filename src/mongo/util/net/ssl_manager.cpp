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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_manager.h"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/tss.hpp>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/exit.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/ssl_expiration.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"

#ifdef MONGO_SSL
#include <openssl/evp.h>
#include <openssl/x509v3.h>
#endif

using std::endl;

namespace mongo {

    SSLGlobalParams sslGlobalParams;

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
                _id = _next.fetchAndAdd(1);
            }

            ~SSLThreadInfo() {
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

            static AtomicUInt32 _next;
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

        AtomicUInt32 SSLThreadInfo::_next;
        std::vector<boost::recursive_mutex*> SSLThreadInfo::_mutex;
        boost::thread_specific_ptr<SSLThreadInfo> SSLThreadInfo::_thread;

        ////////////////////////////////////////////////////////////////

        SimpleMutex sslManagerMtx("SSL Manager");
        SSLManagerInterface* theSSLManager = NULL;
        static const int BUFFER_SIZE = 8*1024;
        static const int DATE_LEN = 128;

        struct Params {
            Params(const std::string& pemfile,
                   const std::string& pempwd,
                   const std::string& clusterfile,
                   const std::string& clusterpwd,
                   const std::string& cafile = "",
                   const std::string& crlfile = "",
                   bool weakCertificateValidation = false,
                   bool allowInvalidCertificates = false,
                   bool allowInvalidHostnames = false,
                   bool fipsMode = false) :
                pemfile(pemfile),
                pempwd(pempwd),
                clusterfile(clusterfile),
                clusterpwd(clusterpwd),
                cafile(cafile),
                crlfile(crlfile),
                weakCertificateValidation(weakCertificateValidation),
                allowInvalidCertificates(allowInvalidCertificates),
                allowInvalidHostnames(allowInvalidHostnames),
                fipsMode(fipsMode) {};

            std::string pemfile;
            std::string pempwd;
            std::string clusterfile;
            std::string clusterpwd;
            std::string cafile;
            std::string crlfile;
            bool weakCertificateValidation;
            bool allowInvalidCertificates;
            bool allowInvalidHostnames;
            bool fipsMode;
        };

        class SSLManager : public SSLManagerInterface {
        public:
            explicit SSLManager(const Params& params, bool isServer);

            virtual ~SSLManager();

            virtual SSLConnection* connect(Socket* socket);

            virtual SSLConnection* accept(Socket* socket, const char* initialBytes, int len);

            virtual std::string parseAndValidatePeerCertificate(const SSLConnection* conn,
                                                                const std::string& remoteHost);

            virtual void cleanupThreadLocals();

            virtual const SSLConfiguration& getSSLConfiguration() const {
                return _sslConfiguration;
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
            bool _weakValidation;
            bool _allowInvalidCertificates;
            bool _allowInvalidHostnames;
            SSLConfiguration _sslConfiguration;

            /**
             * creates an SSL object to be used for this file descriptor.
             * caller must SSL_free it.
             */
            SSL* _secure(SSL_CTX* context, int fd);

            /**
             * Given an error code from an SSL-type IO function, logs an
             * appropriate message and throws a SocketException
             */
            MONGO_COMPILER_NORETURN void _handleSSLError(int code, int ret);

            /*
             * Init the SSL context using parameters provided in params.
             */
            bool _initSSLContext(SSL_CTX** context, const Params& params);

            /*
             * Converts time from OpenSSL return value to unsigned long long
             * representing the milliseconds since the epoch.
             */
            unsigned long long _convertASN1ToMillis(ASN1_TIME* t);

            /*
             * Parse and store x509 subject name from the PEM keyfile.
             * For server instances check that PEM certificate is not expired
             * and extract server certificate notAfter date.
             * @param keyFile referencing the PEM file to be read.
             * @param subjectName as a pointer to the subject name variable being set.
             * @param serverNotAfter a Date_t object pointer that is valued if the
             * date is to be checked (as for a server certificate) and null otherwise.
             * @return bool showing if the function was successful.
             */
            bool _parseAndValidateCertificate(const std::string& keyFile,
                                              std::string* subjectName,
                                              Date_t* serverNotAfter);

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

            /*
             * match a remote host name to an x.509 host name
             */
            bool _hostNameMatch(const char* nameToMatch, const char* certHostName);
            
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
        if (sslGlobalParams.sslMode.load() != SSLGlobalParams::SSLMode_disabled) {
            const Params params(
                sslGlobalParams.sslPEMKeyFile,
                sslGlobalParams.sslPEMKeyPassword,
                sslGlobalParams.sslClusterFile,
                sslGlobalParams.sslClusterPassword,
                sslGlobalParams.sslCAFile,
                sslGlobalParams.sslCRLFile,
                sslGlobalParams.sslWeakCertificateValidation,
                sslGlobalParams.sslAllowInvalidCertificates,
                sslGlobalParams.sslAllowInvalidHostnames,
                sslGlobalParams.sslFIPSMode);
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

    SSLConnection::SSLConnection(SSL_CTX* context, 
                                 Socket* sock, 
                                 const char* initialBytes, 
                                 int len) : socket(sock) {
        // This just ensures that SSL multithreading support is set up for this thread,
        // if it's not already.
        SSLThreadInfo::get();

        ssl = SSL_new(context);
 
        std::string sslErr = NULL != getSSLManager() ? 
            getSSLManager()->getSSLErrorMessage(ERR_get_error()) : "";
        massert(15861, "Error creating new SSL object " + sslErr, ssl);

        BIO_new_bio_pair(&internalBIO, BUFFER_SIZE, &networkBIO, BUFFER_SIZE);
        SSL_set_bio(ssl, internalBIO, internalBIO);

        if (len > 0) {
            int toBIO = BIO_write(networkBIO, initialBytes, len);
            if (toBIO != len) {
                LOG(3) << "Failed to write initial network data to the SSL BIO layer";
                throw SocketException(SocketException::RECV_ERROR , socket->remoteString());
            }
        }
    }

    SSLConnection::~SSLConnection() {
        if (ssl) {   // The internalBIO is automatically freed as part of SSL_free
            SSL_free(ssl);
        }
        if (networkBIO) {
            BIO_free(networkBIO);
        }
    }

    BSONObj SSLConfiguration::getServerStatusBSON() const {
        BSONObjBuilder security;
        security.append("SSLServerSubjectName",
                        serverSubjectName);
        security.appendBool("SSLServerHasCertificateAuthority",
                            hasCA);
        security.appendDate("SSLServerCertificateExpirationDate",
                            serverCertificateExpirationDate);
        return security.obj();
    }

    SSLManagerInterface::~SSLManagerInterface() {}

    SSLManager::SSLManager(const Params& params, bool isServer) :
        _serverContext(NULL),
        _clientContext(NULL),
        _weakValidation(params.weakCertificateValidation),
        _allowInvalidCertificates(params.allowInvalidCertificates),
        _allowInvalidHostnames(params.allowInvalidHostnames) {

        SSL_library_init();
        SSL_load_error_strings();
        ERR_load_crypto_strings();

        if (params.fipsMode) {
            _setupFIPS();
        }

        // Add all digests and ciphers to OpenSSL's internal table
        // so that encryption/decryption is backwards compatible
        OpenSSL_add_all_algorithms();
 
        // Setup OpenSSL multithreading callbacks
        CRYPTO_set_id_callback(_ssl_id_callback);
        CRYPTO_set_locking_callback(_ssl_locking_callback);
 
        SSLThreadInfo::init();
        SSLThreadInfo::get();
 
        if (!_initSSLContext(&_clientContext, params)) {
            uasserted(16768, "ssl initialization problem"); 
        }

        // pick the certificate for use in outgoing connections,
        std::string clientPEM;
        if (!isServer || params.clusterfile.empty()) {
            // We are either a client, or a server without a cluster key,
            // so use the PEM key file, if specified
            clientPEM = params.pemfile;
        }
        else {
            // We are a server with a cluster key, so use the cluster key file
            clientPEM = params.clusterfile;
        }

        if (!clientPEM.empty()) {
            if (!_parseAndValidateCertificate(clientPEM,
                                              &_sslConfiguration.clientSubjectName, NULL)) {
                uasserted(16941, "ssl initialization problem");
            }
        }
        // SSL server specific initialization
        if (isServer) {
            if (!_initSSLContext(&_serverContext, params)) {
                uasserted(16562, "ssl initialization problem");
            }

            if (!_parseAndValidateCertificate(params.pemfile,
                                              &_sslConfiguration.serverSubjectName,
                                              &_sslConfiguration.serverCertificateExpirationDate)) {
                uasserted(16942, "ssl initialization problem");
            }

            static CertificateExpirationMonitor task =
                CertificateExpirationMonitor(_sslConfiguration.serverCertificateExpirationDate);
        }
    }

    SSLManager::~SSLManager() {
        CRYPTO_set_id_callback(0);
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
        // Unless OpenSSL misbehaves, num should always be positive
        fassert(17314, num > 0);
        SSLManager* sm = static_cast<SSLManager*>(userdata);
        const size_t copied = sm->_password.copy(buf, num - 1);
        buf[copied] = '\0';
        return copied;
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
            _handleSSLError(SSL_get_error(conn, status), status);
        return status;
    }

    int SSLManager::SSL_write(SSLConnection* conn, const void* buf, int num) {
        int status;
        do {
            status = ::SSL_write(conn->ssl, buf, num);
        } while(!_doneWithSSLOp(conn, status));
 
        if (status <= 0)
            _handleSSLError(SSL_get_error(conn, status), status);
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
            _handleSSLError(SSL_get_error(conn, status), status);
        return status;
    }

    void SSLManager::SSL_free(SSLConnection* conn) {
        return ::SSL_free(conn->ssl);
    }

    void SSLManager::_setupFIPS() {
        // Turn on FIPS mode if requested.
        // OPENSSL_FIPS must be defined by the OpenSSL headers, plus MONGO_SSL_FIPS
        // must be defined via a MongoDB build flag.
#if defined(OPENSSL_FIPS) && defined(MONGO_SSL_FIPS)
        int status = FIPS_mode_set(1);
        if (!status) {
            severe() << "can't activate FIPS mode: " << 
                getSSLErrorMessage(ERR_get_error()) << endl;
            fassertFailedNoTrace(16703);
        }
        log() << "FIPS 140-2 mode activated" << endl;
#else
        severe() << "this version of mongodb was not compiled with FIPS support";
        fassertFailedNoTrace(17089);
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
        // SSL_OP_NO_SSLv3 - Disable SSL v3 support
        SSL_CTX_set_options(*context, SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

        // HIGH - Enable strong ciphers
        // !EXPORT - Disable export ciphers (40/56 bit) 
        // !aNULL - Disable anonymous auth ciphers
        // @STRENGTH - Sort ciphers based on strength 
        SSL_CTX_set_cipher_list(*context, "HIGH:!EXPORT:!aNULL@STRENGTH");

        // If renegotiation is needed, don't return from recv() or send() until it's successful.
        // Note: this is for blocking sockets only.
        SSL_CTX_set_mode(*context, SSL_MODE_AUTO_RETRY);

        // Disable session caching (see SERVER-10261)
        SSL_CTX_set_session_cache_mode(*context, SSL_SESS_CACHE_OFF);
 
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

    unsigned long long SSLManager::_convertASN1ToMillis(ASN1_TIME* asn1time) {
        BIO *outBIO = BIO_new(BIO_s_mem());
        int timeError = ASN1_TIME_print(outBIO, asn1time);
        ON_BLOCK_EXIT(BIO_free, outBIO);

        if (timeError <= 0) {
            error() << "ASN1_TIME_print failed or wrote no data.";
            return 0;
        }

        char dateChar[DATE_LEN];
        timeError = BIO_gets(outBIO, dateChar, DATE_LEN);
        if (timeError <= 0) {
            error() << "BIO_gets call failed to transfer contents to buf";
            return 0;
        }

        //Ensure that day format is two digits for parsing.
        //Jun  8 17:00:03 2014 becomes Jun 08 17:00:03 2014.
        if (dateChar[4] == ' ') {
            dateChar[4] = '0';
        }

        std::istringstream inStringStream((std::string(dateChar,20)));
        boost::posix_time::time_input_facet *inputFacet = 
            new boost::posix_time::time_input_facet("%b %d %H:%M:%S %Y");

        inStringStream.imbue(std::locale(std::cout.getloc(), inputFacet));
        boost::posix_time::ptime posixTime;
        inStringStream >> posixTime;

        const boost::gregorian::date epoch =
            boost::gregorian::date(1970, boost::gregorian::Jan, 1);

        return (posixTime - boost::posix_time::ptime(epoch)).total_milliseconds();
    }

    bool SSLManager::_parseAndValidateCertificate(const std::string& keyFile, 
                                                  std::string* subjectName,
                                                  Date_t* serverCertificateExpirationDate) {
        BIO *inBIO = BIO_new(BIO_s_file_internal());
        if (inBIO == NULL) {
            error() << "failed to allocate BIO object: "
                    << getSSLErrorMessage(ERR_get_error());
            return false;
        }

        ON_BLOCK_EXIT(BIO_free, inBIO);
        if (BIO_read_filename(inBIO, keyFile.c_str()) <= 0) {
            error() << "cannot read key file when setting subject name: "
                    << keyFile << ' ' << getSSLErrorMessage(ERR_get_error());
            return false;
        }

        X509* x509 = PEM_read_bio_X509(inBIO, NULL, &SSLManager::password_cb, this);
        if (x509 == NULL) {
            error() << "cannot retrieve certificate from keyfile: "
                    << keyFile << ' ' << getSSLErrorMessage(ERR_get_error()); 
            return false;
        }
        ON_BLOCK_EXIT(X509_free, x509);

        *subjectName = getCertificateSubjectName(x509);
        if (serverCertificateExpirationDate != NULL) {

            unsigned long long notBeforeMillis = _convertASN1ToMillis(X509_get_notBefore(x509));
            if (notBeforeMillis == 0) {
                error() << "date conversion failed";
                return false;
            }

            unsigned long long notAfterMillis = _convertASN1ToMillis(X509_get_notAfter(x509));
            if (notAfterMillis == 0) {
                error() << "date conversion failed";
                return false;
            }

            if ((notBeforeMillis > curTimeMillis64()) ||
                (curTimeMillis64() > notAfterMillis)) {
                dbexit(EXIT_BADOPTIONS,
                       "The provided SSL certificate is expired or not yet valid.");
            }

            *serverCertificateExpirationDate = Date_t(notAfterMillis);
        }

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
        // Set the list of CAs sent to clients
        STACK_OF (X509_NAME) * certNames = SSL_load_client_CA_file(caFile.c_str());
        if (certNames == NULL) {
            error() << "cannot read certificate authority file: " << caFile << " " <<
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        SSL_CTX_set_client_CA_list(context, certNames);

        // Load trusted CA
        if (SSL_CTX_load_verify_locations(context, caFile.c_str(), NULL) != 1) {
            error() << "cannot read certificate authority file: " << caFile << " " <<
                getSSLErrorMessage(ERR_get_error()) << endl;
            return false;
        }
        // Set SSL to require peer (client) certificate verification
        // if a certificate is presented
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, &SSLManager::verify_cb);
        _sslConfiguration.hasCA = true;
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
        SSLConnection* sslConn = new SSLConnection(_clientContext, socket, NULL, 0);
        ScopeGuard sslGuard = MakeGuard(::SSL_free, sslConn->ssl);
        ScopeGuard bioGuard = MakeGuard(::BIO_free, sslConn->networkBIO);
 
        int ret;
        do {
            ret = ::SSL_connect(sslConn->ssl);
        } while(!_doneWithSSLOp(sslConn, ret));
 
        if (ret != 1)
            _handleSSLError(SSL_get_error(sslConn, ret), ret);
 
        sslGuard.Dismiss();
        bioGuard.Dismiss();
        return sslConn;
    }

    SSLConnection* SSLManager::accept(Socket* socket, const char* initialBytes, int len) {
        SSLConnection* sslConn = new SSLConnection(_serverContext, socket, initialBytes, len);
        ScopeGuard sslGuard = MakeGuard(::SSL_free, sslConn->ssl);
        ScopeGuard bioGuard = MakeGuard(::BIO_free, sslConn->networkBIO);
 
        int ret;
        do {
            ret = ::SSL_accept(sslConn->ssl);
        } while(!_doneWithSSLOp(sslConn, ret));
 
        if (ret != 1)
            _handleSSLError(SSL_get_error(sslConn, ret), ret);
 
        sslGuard.Dismiss();
        bioGuard.Dismiss();
        return sslConn;
    }

    // TODO SERVER-11601 Use NFC Unicode canonicalization
    bool SSLManager::_hostNameMatch(const char* nameToMatch, 
                                    const char* certHostName) {
        if (strlen(certHostName) < 2) {
            return false;
        }
        
        // match wildcard DNS names
        if (certHostName[0] == '*' && certHostName[1] == '.') {
            // allow name.example.com if the cert is *.example.com, '*' does not match '.'
            const char* subName = strchr(nameToMatch, '.');
            return subName && !strcasecmp(certHostName+1, subName);
        }
        else {
            return !strcasecmp(nameToMatch, certHostName);
        }
    }

    std::string SSLManager::parseAndValidatePeerCertificate(const SSLConnection* conn, 
                                                    const std::string& remoteHost) {
        // only set if a CA cert has been provided
        if (!_sslConfiguration.hasCA) return "";

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
            if (_allowInvalidCertificates) {
                warning() << "SSL peer certificate validation failed:" << 
                    X509_verify_cert_error_string(result);
            }
            else {
                error() <<  "SSL peer certificate validation failed:" << 
                    X509_verify_cert_error_string(result);
                throw SocketException(SocketException::CONNECT_ERROR, "");
            }
        }
 
        // TODO: check optional cipher restriction, using cert.
        std::string peerSubjectName = getCertificateSubjectName(peerCert);

        // If this is an SSL client context (on a MongoDB server or client) 
        // perform hostname validation of the remote server 
        if (remoteHost.empty()) {
            return peerSubjectName;
        }

        // Try to match using the Subject Alternate Name, if it exists.
        // RFC-2818 requires the Subject Alternate Name to be used if present.
        // Otherwise, the most specific Common Name field in the subject field
        // must be used.

        bool sanMatch = false;
        bool cnMatch = false;

        STACK_OF(GENERAL_NAME)* sanNames = static_cast<STACK_OF(GENERAL_NAME)*>
            (X509_get_ext_d2i(peerCert, NID_subject_alt_name, NULL, NULL));

        if (sanNames != NULL) {
            int sanNamesList = sk_GENERAL_NAME_num(sanNames);
            for (int i = 0; i < sanNamesList; i++) {
                const GENERAL_NAME* currentName = sk_GENERAL_NAME_value(sanNames, i);
                if (currentName && currentName->type == GEN_DNS) {
                    char *dnsName =
                        reinterpret_cast<char *>(ASN1_STRING_data(currentName->d.dNSName));
                    if (_hostNameMatch(remoteHost.c_str(), dnsName)) {
                        sanMatch = true;
                        break;
                    }
                }
            }
            sk_GENERAL_NAME_pop_free(sanNames, GENERAL_NAME_free);
        }
        else {
            // If Subject Alternate Name (SAN) didn't exist, check Common Name (CN).
            int cnBegin = peerSubjectName.find("CN=") + 3;
            int cnEnd = peerSubjectName.find(",", cnBegin);
            std::string commonName = peerSubjectName.substr(cnBegin, cnEnd-cnBegin);

            if (_hostNameMatch(remoteHost.c_str(), commonName.c_str())) {
                cnMatch = true;
            }
        }

        if (!sanMatch && !cnMatch) {
            if (_allowInvalidCertificates || _allowInvalidHostnames) {
                warning() << "The server certificate does not match the host name " <<
                    remoteHost;
            }
            else {
                error() << "The server certificate does not match the host name " <<
                    remoteHost;
                throw SocketException(SocketException::CONNECT_ERROR, "");
            }
        }

        return peerSubjectName;
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

    void SSLManager::_handleSSLError(int code, int ret) {
        int err = ERR_get_error();
        
        switch (code) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // should not happen because we turned on AUTO_RETRY
            // However, it turns out this CAN happen during a connect, if the other side
            // accepts the socket connection but fails to do the SSL handshake in a timely
            // manner.
            error() << "SSL: " << code << ", possibly timed out during connect";
            break;

        case SSL_ERROR_ZERO_RETURN: 
            // TODO: Check if we can avoid throwing an exception for this condition
            LOG(3) << "SSL network connection closed";
            break;
        case SSL_ERROR_SYSCALL:
            // If ERR_get_error returned 0, the error queue is empty
            // check the return value of the actual SSL operation
            if (err != 0) {
                error() << "SSL: " << getSSLErrorMessage(err);
            }
            else if (ret == 0) {
                error() << "Unexpected EOF encountered during SSL communication";
            }
            else {
                error() << "The SSL BIO reported an I/O error " << errnoWithDescription();
            }
            break;
        case SSL_ERROR_SSL:
        {
            error() << "SSL: " << getSSLErrorMessage(err);
            break;
        }
        
        default:
            error() << "unrecognized SSL error";
            break;
        }
        throw SocketException(SocketException::CONNECT_ERROR, "");
    }
#endif // #ifdef MONGO_SSL
}
