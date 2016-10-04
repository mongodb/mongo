/* Copyright 2013 10gen Inc.
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

#pragma once

#include "mongo/util/net/ssl_manager.h"

#include <vector>

#include "mongo/base/status.h"

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

struct SSLParams {
    enum class Protocols { TLS1_0, TLS1_1, TLS1_2 };
    AtomicInt32 sslMode;             // --sslMode - the SSL operation mode, see enum SSLModes
    std::string sslPEMKeyFile;       // --sslPEMKeyFile
    std::string sslPEMKeyPassword;   // --sslPEMKeyPassword
    std::string sslClusterFile;      // --sslInternalKeyFile
    std::string sslClusterPassword;  // --sslInternalKeyPassword
    std::string sslCAFile;           // --sslCAFile
    std::string sslCRLFile;          // --sslCRLFile
    std::string sslCipherConfig;     // --sslCipherConfig
    std::vector<Protocols> sslDisabledProtocols;  // --sslDisabledProtocols
    bool sslWeakCertificateValidation = false;    // --sslWeakCertificateValidation
    bool sslFIPSMode = false;                     // --sslFIPSMode
    bool sslAllowInvalidCertificates = false;     // --sslAllowInvalidCertificates
    bool sslAllowInvalidHostnames = false;        // --sslAllowInvalidHostnames

    SSLParams() {
        sslMode.store(SSLMode_disabled);
    }

    enum SSLModes {
        /**
        * Make unencrypted outgoing connections and do not accept incoming SSL-connections
        */
        SSLMode_disabled,

        /**
        * Make unencrypted outgoing connections and accept both unencrypted and SSL-connections
        */
        SSLMode_allowSSL,

        /**
        * Make outgoing SSL-connections and accept both unecrypted and SSL-connections
        */
        SSLMode_preferSSL,

        /**
        * Make outgoing SSL-connections and only accept incoming SSL-connections
        */
        SSLMode_requireSSL
    };
};

extern SSLParams sslGlobalParams;

Status addSSLServerOptions(moe::OptionSection* options);

Status addSSLClientOptions(moe::OptionSection* options);

Status storeSSLServerOptions(const moe::Environment& params);

/**
 * Canonicalize SSL options for the given environment that have different representations with
 * the same logical meaning
 */
Status canonicalizeSSLServerOptions(moe::Environment* params);

Status validateSSLServerOptions(const moe::Environment& params);

Status storeSSLClientOptions(const moe::Environment& params);
}
