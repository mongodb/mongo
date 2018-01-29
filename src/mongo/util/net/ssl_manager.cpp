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

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_manager.h"

#include <boost/algorithm/string.hpp>
#include <string>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/server_parameters.h"
#include "mongo/transport/session.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/text.h"


namespace mongo {

namespace {

const transport::Session::Decoration<SSLPeerInfo> peerInfoForSession =
    transport::Session::declareDecoration<SSLPeerInfo>();

/**
 * Configurable via --setParameter disableNonSSLConnectionLogging=true. If false (default)
 * if the sslMode is set to preferSSL, we will log connections that are not using SSL.
 * If true, such log messages will be suppressed.
 */
ExportedServerParameter<bool, ServerParameterType::kStartupOnly>
    disableNonSSLConnectionLoggingParameter(ServerParameterSet::getGlobal(),
                                            "disableNonSSLConnectionLogging",
                                            &sslGlobalParams.disableNonSSLConnectionLogging);

ExportedServerParameter<std::string, ServerParameterType::kStartupOnly>
    setDiffieHellmanParameterPEMFile(ServerParameterSet::getGlobal(),
                                     "opensslDiffieHellmanParameters",
                                     &sslGlobalParams.sslPEMTempDHParam);
}  // namespace

SSLPeerInfo& SSLPeerInfo::forSession(const transport::SessionHandle& session) {
    return peerInfoForSession(session.get());
}

SSLParams sslGlobalParams;

const SSLParams& getSSLGlobalParams() {
    return sslGlobalParams;
}

class OpenSSLCipherConfigParameter
    : public ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> {
public:
    OpenSSLCipherConfigParameter()
        : ExportedServerParameter<std::string, ServerParameterType::kStartupOnly>(
              ServerParameterSet::getGlobal(),
              "opensslCipherConfig",
              &sslGlobalParams.sslCipherConfig) {}
    Status validate(const std::string& potentialNewValue) final {
        if (!sslGlobalParams.sslCipherConfig.empty()) {
            return Status(
                ErrorCodes::BadValue,
                "opensslCipherConfig setParameter is incompatible with net.ssl.sslCipherConfig");
        }
        // Note that there is very little validation that we can do here.
        // OpenSSL exposes no API to validate a cipher config string. The only way to figure out
        // what a string maps to is to make an SSL_CTX object, set the string on it, then parse the
        // resulting STACK_OF object. If provided an invalid entry in the string, it will silently
        // ignore it. Because an entry in the string may map to multiple ciphers, or remove ciphers
        // from the final set produced by the full string, we can't tell if any entry failed
        // to parse.
        return Status::OK();
    }
} openSSLCipherConfig;

#ifdef MONGO_CONFIG_SSL

namespace {
void canonicalizeClusterDN(std::vector<std::string>* dn) {
    // remove all RDNs we don't care about
    for (size_t i = 0; i < dn->size(); i++) {
        std::string& comp = dn->at(i);
        boost::algorithm::trim(comp);
        if (!mongoutils::str::startsWith(comp.c_str(), "DC=") &&
            !mongoutils::str::startsWith(comp.c_str(), "O=") &&
            !mongoutils::str::startsWith(comp.c_str(), "OU=")) {
            dn->erase(dn->begin() + i);
            i--;
        }
    }
    std::stable_sort(dn->begin(), dn->end());
}
}  // namespace

bool SSLConfiguration::isClusterMember(StringData subjectName) const {
    std::vector<std::string> clientRDN = StringSplitter::split(subjectName.toString(), ",");
    std::vector<std::string> serverRDN = StringSplitter::split(serverSubjectName, ",");

    canonicalizeClusterDN(&clientRDN);
    canonicalizeClusterDN(&serverRDN);

    if (clientRDN.size() == 0 || clientRDN.size() != serverRDN.size()) {
        return false;
    }

    for (size_t i = 0; i < serverRDN.size(); i++) {
        if (clientRDN[i] != serverRDN[i]) {
            return false;
        }
    }
    return true;
}

BSONObj SSLConfiguration::getServerStatusBSON() const {
    BSONObjBuilder security;
    security.append("SSLServerSubjectName", serverSubjectName);
    security.appendBool("SSLServerHasCertificateAuthority", hasCA);
    security.appendDate("SSLServerCertificateExpirationDate", serverCertificateExpirationDate);
    return security.obj();
}

SSLManagerInterface::~SSLManagerInterface() {}

SSLConnectionInterface::~SSLConnectionInterface() {}

#endif

}  // namespace mongo
