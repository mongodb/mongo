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

namespace mongo {
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
    

    SSLManager::SSLManager(bool client) {
        _client = client;
        SSL_library_init();
        SSL_load_error_strings();
        ERR_load_crypto_strings();
        
        _context = SSL_CTX_new( client ? SSLv23_client_method() : SSLv23_server_method() );
        massert( 15864 , mongoutils::str::stream() << "can't create SSL Context: " 
                 << ERR_error_string(ERR_get_error(), NULL) , _context );
        
        SSL_CTX_set_options( _context, SSL_OP_ALL);   
        SSLThreadInfo::init();
        SSLThreadInfo::get();
    }

    void SSLManager::setupPubPriv(const std::string& privateKeyFile, const std::string& publicKeyFile) {
        massert(15865, 
                mongoutils::str::stream() << "Can't read SSL certificate from file " 
                << publicKeyFile << ":" <<  ERR_error_string(ERR_get_error(), NULL) ,
                SSL_CTX_use_certificate_file(_context, publicKeyFile.c_str(), SSL_FILETYPE_PEM));
  

        massert(15866 , 
                 mongoutils::str::stream() << "Can't read SSL private key from file " 
                 << privateKeyFile << " : " << ERR_error_string(ERR_get_error(), NULL) ,
                 SSL_CTX_use_PrivateKey_file(_context, privateKeyFile.c_str(), SSL_FILETYPE_PEM));
    }
    
    
    int SSLManager::password_cb(char *buf,int num, int rwflag,void *userdata) {
        SSLManager* sm = static_cast<SSLManager*>(userdata);
        std::string pass = sm->_password;
        strcpy(buf,pass.c_str());
        return(pass.size());
    }

    bool SSLManager::setupPEM(const std::string& keyFile , const std::string& password) {
        _password = password;
        
        if ( SSL_CTX_use_certificate_chain_file( _context , keyFile.c_str() ) != 1 ) {
            log() << "Can't read certificate file: " << keyFile << endl;
            return false;
        }
        
        SSL_CTX_set_default_passwd_cb_userdata( _context , this );
        SSL_CTX_set_default_passwd_cb( _context, &SSLManager::password_cb );
        
        if ( SSL_CTX_use_PrivateKey_file( _context , keyFile.c_str() , SSL_FILETYPE_PEM ) != 1 ) {
            log() << "Can't read key file: " << keyFile << endl;
            return false;
        }
        
        return true;
    }
        
    SSL * SSLManager::secure(int fd) {
        SSLThreadInfo::get();
        SSL * ssl = SSL_new(_context);
        massert( 15861 , "can't create SSL" , ssl );
        SSL_set_fd( ssl , fd );
        return ssl;
    }
}
#endif
