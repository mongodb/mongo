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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_options.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/text.h"

namespace mongo {

using std::string;

Status addSSLServerOptions(moe::OptionSection* options) {
    options
        ->addOptionChaining("net.ssl.sslOnNormalPorts",
                            "sslOnNormalPorts",
                            moe::Switch,
                            "use ssl on configured ports")
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("net.ssl.mode");

    options->addOptionChaining(
        "net.ssl.mode",
        "sslMode",
        moe::String,
        "set the SSL operation mode (disabled|allowSSL|preferSSL|requireSSL)");

    options->addOptionChaining(
        "net.ssl.PEMKeyFile", "sslPEMKeyFile", moe::String, "PEM file for ssl");

    options
        ->addOptionChaining(
            "net.ssl.PEMKeyPassword", "sslPEMKeyPassword", moe::String, "PEM file password")
        .setImplicit(moe::Value(std::string("")));

    options->addOptionChaining("net.ssl.clusterFile",
                               "sslClusterFile",
                               moe::String,
                               "Key file for internal SSL authentication");

    options
        ->addOptionChaining("net.ssl.clusterPassword",
                            "sslClusterPassword",
                            moe::String,
                            "Internal authentication key file password")
        .setImplicit(moe::Value(std::string("")));

    options->addOptionChaining(
        "net.ssl.CAFile", "sslCAFile", moe::String, "Certificate Authority file for SSL");

    options->addOptionChaining(
        "net.ssl.CRLFile", "sslCRLFile", moe::String, "Certificate Revocation List file for SSL");

    options
        ->addOptionChaining("net.ssl.sslCipherConfig",
                            "sslCipherConfig",
                            moe::String,
                            "OpenSSL cipher configuration string")
        .hidden();

    options->addOptionChaining(
        "net.ssl.disabledProtocols",
        "sslDisabledProtocols",
        moe::String,
        "Comma separated list of TLS protocols to disable [TLS1_0,TLS1_1,TLS1_2]");

    options->addOptionChaining("net.ssl.weakCertificateValidation",
                               "sslWeakCertificateValidation",
                               moe::Switch,
                               "allow client to connect without "
                               "presenting a certificate");

    // Alias for --sslWeakCertificateValidation.
    options->addOptionChaining("net.ssl.allowConnectionsWithoutCertificates",
                               "sslAllowConnectionsWithoutCertificates",
                               moe::Switch,
                               "allow client to connect without presenting a certificate");

    options->addOptionChaining("net.ssl.allowInvalidHostnames",
                               "sslAllowInvalidHostnames",
                               moe::Switch,
                               "Allow server certificates to provide non-matching hostnames");

    options->addOptionChaining("net.ssl.allowInvalidCertificates",
                               "sslAllowInvalidCertificates",
                               moe::Switch,
                               "allow connections to servers with invalid certificates");

    options->addOptionChaining(
        "net.ssl.FIPSMode", "sslFIPSMode", moe::Switch, "activate FIPS 140-2 mode at startup");

    return Status::OK();
}

Status addSSLClientOptions(moe::OptionSection* options) {
    options->addOptionChaining("ssl", "ssl", moe::Switch, "use SSL for all connections");

    options
        ->addOptionChaining(
            "ssl.CAFile", "sslCAFile", moe::String, "Certificate Authority file for SSL")
        .requires("ssl");

    options
        ->addOptionChaining(
            "ssl.PEMKeyFile", "sslPEMKeyFile", moe::String, "PEM certificate/key file for SSL")
        .requires("ssl");

    options
        ->addOptionChaining("ssl.PEMKeyPassword",
                            "sslPEMKeyPassword",
                            moe::String,
                            "password for key in PEM file for SSL")
        .requires("ssl");

    options
        ->addOptionChaining(
            "ssl.CRLFile", "sslCRLFile", moe::String, "Certificate Revocation List file for SSL")
        .requires("ssl")
        .requires("ssl.CAFile");

    options
        ->addOptionChaining("net.ssl.allowInvalidHostnames",
                            "sslAllowInvalidHostnames",
                            moe::Switch,
                            "allow connections to servers with non-matching hostnames")
        .requires("ssl");

    options
        ->addOptionChaining("ssl.allowInvalidCertificates",
                            "sslAllowInvalidCertificates",
                            moe::Switch,
                            "allow connections to servers with invalid certificates")
        .requires("ssl");

    options->addOptionChaining(
        "ssl.FIPSMode", "sslFIPSMode", moe::Switch, "activate FIPS 140-2 mode at startup");

    return Status::OK();
}

Status validateSSLServerOptions(const moe::Environment& params) {
#ifdef _WIN32
    if (params.count("install") || params.count("reinstall")) {
        if (params.count("net.ssl.PEMKeyFile") &&
            !boost::filesystem::path(params["net.ssl.PEMKeyFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "PEMKeyFile requires an absolute file path with Windows services");
        }

        if (params.count("net.ssl.clusterFile") &&
            !boost::filesystem::path(params["net.ssl.clusterFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "clusterFile requires an absolute file path with Windows services");
        }

        if (params.count("net.ssl.CAFile") &&
            !boost::filesystem::path(params["net.ssl.CAFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "CAFile requires an absolute file path with Windows services");
        }

        if (params.count("net.ssl.CRLFile") &&
            !boost::filesystem::path(params["net.ssl.CRLFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "CRLFile requires an absolute file path with Windows services");
        }
    }
#endif

    return Status::OK();
}

Status canonicalizeSSLServerOptions(moe::Environment* params) {
    if (params->count("net.ssl.sslOnNormalPorts") &&
        (*params)["net.ssl.sslOnNormalPorts"].as<bool>() == true) {
        Status ret = params->set("net.ssl.mode", moe::Value(std::string("requireSSL")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("net.ssl.sslOnNormalPorts");
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

Status storeSSLServerOptions(const moe::Environment& params) {
    if (params.count("net.ssl.mode")) {
        std::string sslModeParam = params["net.ssl.mode"].as<string>();
        if (sslModeParam == "disabled") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_disabled);
        } else if (sslModeParam == "allowSSL") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_allowSSL);
        } else if (sslModeParam == "preferSSL") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_preferSSL);
        } else if (sslModeParam == "requireSSL") {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
        } else {
            return Status(ErrorCodes::BadValue, "unsupported value for sslMode " + sslModeParam);
        }
    }

    if (params.count("net.ssl.PEMKeyFile")) {
        sslGlobalParams.sslPEMKeyFile =
            boost::filesystem::absolute(params["net.ssl.PEMKeyFile"].as<string>()).generic_string();
    }

    if (params.count("net.ssl.PEMKeyPassword")) {
        sslGlobalParams.sslPEMKeyPassword = params["net.ssl.PEMKeyPassword"].as<string>();
    }

    if (params.count("net.ssl.clusterFile")) {
        sslGlobalParams.sslClusterFile =
            boost::filesystem::absolute(params["net.ssl.clusterFile"].as<string>())
                .generic_string();
    }

    if (params.count("net.ssl.clusterPassword")) {
        sslGlobalParams.sslClusterPassword = params["net.ssl.clusterPassword"].as<string>();
    }

    if (params.count("net.ssl.CAFile")) {
        sslGlobalParams.sslCAFile =
            boost::filesystem::absolute(params["net.ssl.CAFile"].as<std::string>())
                .generic_string();
    }

    if (params.count("net.ssl.CRLFile")) {
        sslGlobalParams.sslCRLFile =
            boost::filesystem::absolute(params["net.ssl.CRLFile"].as<std::string>())
                .generic_string();
    }

    if (params.count("net.ssl.sslCipherConfig")) {
        sslGlobalParams.sslCipherConfig = params["net.ssl.sslCipherConfig"].as<string>();
    }

    if (params.count("net.ssl.disabledProtocols")) {
        // The disabledProtocols field is composed of a comma separated list of protocols to
        // disable. First, tokenize the field.
        std::vector<std::string> tokens =
            StringSplitter::split(params["net.ssl.disabledProtocols"].as<string>(), ",");

        // All accepted tokens, and their corresponding enum representation. The noTLS* tokens
        // exist for backwards compatibility.
        const std::map<std::string, SSLParams::Protocols> validConfigs{
            {"TLS1_0", SSLParams::Protocols::TLS1_0},
            {"noTLS1_0", SSLParams::Protocols::TLS1_0},
            {"TLS1_1", SSLParams::Protocols::TLS1_1},
            {"noTLS1_1", SSLParams::Protocols::TLS1_1},
            {"TLS1_2", SSLParams::Protocols::TLS1_2},
            {"noTLS1_2", SSLParams::Protocols::TLS1_2}};

        // Map the tokens to their enum values, and push them onto the list of disabled protocols.
        for (const std::string& token : tokens) {
            auto mappedToken = validConfigs.find(token);
            if (mappedToken != validConfigs.end()) {
                sslGlobalParams.sslDisabledProtocols.push_back(mappedToken->second);
            } else {
                return Status(ErrorCodes::BadValue,
                              "Unrecognized disabledProtocols '" + token + "'");
            }
        }
    }

    if (params.count("net.ssl.weakCertificateValidation")) {
        sslGlobalParams.sslWeakCertificateValidation =
            params["net.ssl.weakCertificateValidation"].as<bool>();
    } else if (params.count("net.ssl.allowConnectionsWithoutCertificates")) {
        sslGlobalParams.sslWeakCertificateValidation =
            params["net.ssl.allowConnectionsWithoutCertificates"].as<bool>();
    }
    if (params.count("net.ssl.allowInvalidHostnames")) {
        sslGlobalParams.sslAllowInvalidHostnames =
            params["net.ssl.allowInvalidHostnames"].as<bool>();
    }
    if (params.count("net.ssl.allowInvalidCertificates")) {
        sslGlobalParams.sslAllowInvalidCertificates =
            params["net.ssl.allowInvalidCertificates"].as<bool>();
    }
    if (params.count("net.ssl.FIPSMode")) {
        sslGlobalParams.sslFIPSMode = params["net.ssl.FIPSMode"].as<bool>();
    }

    int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();
    if (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled) {
        if (sslGlobalParams.sslPEMKeyFile.size() == 0) {
            return Status(ErrorCodes::BadValue, "need sslPEMKeyFile when SSL is enabled");
        }
        if (!sslGlobalParams.sslCRLFile.empty() && sslGlobalParams.sslCAFile.empty()) {
            return Status(ErrorCodes::BadValue, "need sslCAFile with sslCRLFile");
        }
        std::string sslCANotFoundError(
            "No SSL certificate validation can be performed since"
            " no CA file has been provided; please specify an"
            " sslCAFile parameter");

        if (sslGlobalParams.sslCAFile.empty() &&
            clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
            return Status(ErrorCodes::BadValue, sslCANotFoundError);
        }
    } else if (sslGlobalParams.sslPEMKeyFile.size() || sslGlobalParams.sslPEMKeyPassword.size() ||
               sslGlobalParams.sslClusterFile.size() || sslGlobalParams.sslClusterPassword.size() ||
               sslGlobalParams.sslCAFile.size() || sslGlobalParams.sslCRLFile.size() ||
               sslGlobalParams.sslCipherConfig.size() ||
               sslGlobalParams.sslDisabledProtocols.size() ||
               sslGlobalParams.sslWeakCertificateValidation) {
        return Status(ErrorCodes::BadValue,
                      "need to enable SSL via the sslMode flag when "
                      "using SSL configuration parameters");
    }
    if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
        if (sslGlobalParams.sslMode.load() == SSLParams::SSLMode_disabled) {
            return Status(ErrorCodes::BadValue, "need to enable SSL via the sslMode flag");
        }
    }
    if (sslGlobalParams.sslMode.load() == SSLParams::SSLMode_allowSSL) {
        // allowSSL and x509 is valid only when we are transitioning to auth.
        if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
            (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509 &&
             !serverGlobalParams.transitionToAuth)) {
            return Status(ErrorCodes::BadValue,
                          "cannot have x.509 cluster authentication in allowSSL mode");
        }
    }
    return Status::OK();
}

Status storeSSLClientOptions(const moe::Environment& params) {
    if (params.count("ssl") && params["ssl"].as<bool>() == true) {
        sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
    }
    if (params.count("ssl.PEMKeyFile")) {
        sslGlobalParams.sslPEMKeyFile = params["ssl.PEMKeyFile"].as<std::string>();
    }
    if (params.count("ssl.PEMKeyPassword")) {
        sslGlobalParams.sslPEMKeyPassword = params["ssl.PEMKeyPassword"].as<std::string>();
    }
    if (params.count("ssl.CAFile")) {
        sslGlobalParams.sslCAFile = params["ssl.CAFile"].as<std::string>();
    }
    if (params.count("ssl.CRLFile")) {
        sslGlobalParams.sslCRLFile = params["ssl.CRLFile"].as<std::string>();
    }
    if (params.count("net.ssl.allowInvalidHostnames")) {
        sslGlobalParams.sslAllowInvalidHostnames =
            params["net.ssl.allowInvalidHostnames"].as<bool>();
    }
    if (params.count("ssl.allowInvalidCertificates")) {
        sslGlobalParams.sslAllowInvalidCertificates = true;
    }
    if (params.count("ssl.FIPSMode")) {
        sslGlobalParams.sslFIPSMode = true;
    }
    return Status::OK();
}

}  // namespace mongo
