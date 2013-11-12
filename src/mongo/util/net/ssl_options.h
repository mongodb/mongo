/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/net/ssl_manager.h"

namespace mongo {

    namespace optionenvironment {
        class OptionSection;
        class Environment;
    } // namespace optionenvironment

    namespace moe = mongo::optionenvironment;

    struct SSLGlobalParams {
        AtomicInt32 sslMode;        // --sslMode - the SSL operation mode, see enum SSLModes
        bool sslOnNormalPorts;      // --sslOnNormalPorts (deprecated)
        std::string sslPEMKeyFile;       // --sslPEMKeyFile
        std::string sslPEMKeyPassword;   // --sslPEMKeyPassword
        std::string sslClusterFile;       // --sslInternalKeyFile
        std::string sslClusterPassword;   // --sslInternalKeyPassword
        std::string sslCAFile;      // --sslCAFile
        std::string sslCRLFile;     // --sslCRLFile
        bool sslWeakCertificateValidation; // --sslWeakCertificateValidation
        bool sslFIPSMode; // --sslFIPSMode
        bool sslAllowInvalidCertificates; // --sslIgnoreCertificateValidation

        SSLGlobalParams() {
            sslMode.store(SSLMode_noSSL);
        }
 
        enum SSLModes {
            /** 
            * Make unencrypted outgoing connections and do not accept incoming SSL-connections 
            */
            SSLMode_noSSL,

            /**
            * Make unencrypted outgoing connections and accept both unencrypted and SSL-connections 
            */
            SSLMode_acceptSSL,

            /**
            * Make outgoing SSL-connections and accept both unecrypted and SSL-connections
            */
            SSLMode_sendAcceptSSL,
 
            /**
            * Make outgoing SSL-connections and only accept incoming SSL-connections
            */
            SSLMode_sslOnly
        };
    };

    extern SSLGlobalParams sslGlobalParams;


    Status addSSLServerOptions(moe::OptionSection* options);

    Status addSSLClientOptions(moe::OptionSection* options);

    Status storeSSLServerOptions(const moe::Environment& params);

    Status storeSSLClientOptions(const moe::Environment& params);
}
