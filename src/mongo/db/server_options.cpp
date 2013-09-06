/*
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/server_options.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/cmdline.h" // For CmdLine::DefaultDBPort
#include "mongo/util/net/listen.h" // For DEFAULT_MAX_CONN
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"

namespace mongo {

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addGeneralServerOptions(moe::OptionSection* options) {
        StringBuilder portInfoBuilder;
        StringBuilder maxConnInfoBuilder;

        portInfoBuilder << "specify port number - " << CmdLine::DefaultDBPort << " by default";
        maxConnInfoBuilder << "max number of simultaneous connections - "
                           << DEFAULT_MAX_CONN << " by default";

        Status ret = options->addOption(OD("help", "help,h", moe::Switch,
                    "show this usage information", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("version", "version", moe::Switch, "show version information",
                    true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("config", "config,f", moe::String,
                    "configuration file specifying additional options", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("verbose", "verbose,v", moe::Switch,
                    "be more verbose (include multiple times for more verbosity e.g. -vvvvv)",
                    true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("quiet", "quiet", moe::Switch, "quieter output", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("port", "port", moe::Int, portInfoBuilder.str().c_str(), true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("bind_ip", "bind_ip", moe::String,
                    "comma separated list of ip addresses to listen on - all local ips by default",
                    true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("maxConns", "maxConns", moe::Int,
                    maxConnInfoBuilder.str().c_str(), true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("logpath", "logpath", moe::String,
                    "log file to send write to instead of stdout - has to be a file, not directory",
                    true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("logappend", "logappend", moe::Switch,
                    "append to logpath instead of over-writing", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("logTimestampFormat", "logTimestampFormat", moe::String,
                    "Desired format for timestamps in log messages. One of ctime, "
                    "iso8601-utc or iso8601-local", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("pidfilepath", "pidfilepath", moe::String,
                    "full path to pidfile (if not set, no pidfile is created)", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("keyFile", "keyFile", moe::String,
                    "private key for cluster authentication", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("setParameter", "setParameter", moe::StringVector,
                    "Set a configurable parameter", true, moe::Value(), moe::Value(), true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("httpinterface", "httpinterface", moe::Switch,
                    "enable http interface", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("clusterAuthMode", "clusterAuthMode", moe::String,
                    "Authentication mode used for cluster authentication. Alternatives are "
                    "(keyfile|sendKeyfile|sendX509|x509)", true));
        if (!ret.isOK()) {
            return ret;
        }
#ifndef _WIN32
        ret = options->addOption(OD("nounixsocket", "nounixsocket", moe::Switch,
                    "disable listening on unix sockets", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("unixSocketPrefix", "unixSocketPrefix", moe::String,
                    "alternative directory for UNIX domain sockets (defaults to /tmp)", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("fork", "fork", moe::Switch, "fork server process", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("syslog", "syslog", moe::Switch,
                    "log to system's syslog facility instead of file or stdout", true));
        if (!ret.isOK()) {
            return ret;
        }
#endif

        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            ret = options->addOption(OD(s.c_str(), s.c_str(), moe::Switch, "verbose", false));
            if(!ret.isOK()) {
                return ret;
            }
        }

        // Extra hidden options
        ret = options->addOption(OD("nohttpinterface", "nohttpinterface", moe::Switch,
                    "disable http interface", false));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("objcheck", "objcheck", moe::Switch,
                    "inspect client data for validity on receipt (DEFAULT)", false));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("noobjcheck", "noobjcheck", moe::Switch,
                    "do NOT inspect client data for validity on receipt", false));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("traceExceptions", "traceExceptions", moe::Switch,
                    "log stack traces for every exception", false));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("enableExperimentalIndexStatsCmd",
                    "enableExperimentalIndexStatsCmd", moe::Switch,
                    "EXPERIMENTAL (UNSUPPORTED). "
                    "Enable command computing aggregate statistics on indexes.", false));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("enableExperimentalStorageDetailsCmd",
                    "enableExperimentalStorageDetailsCmd", moe::Switch,
                    "EXPERIMENTAL (UNSUPPORTED). "
                    "Enable command computing aggregate statistics on storage.", false));
        if (!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addWindowsServerOptions(moe::OptionSection* options) {
        Status ret = options->addOption(OD("install", "install", moe::Switch,
                    "install Windows service", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("remove", "remove", moe::Switch, "remove Windows service",
                    true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("reinstall", "reinstall", moe::Switch,
                    "reinstall Windows service (equivalent to --remove followed by --install)",
                    true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("serviceName", "serviceName", moe::String,
                    "Windows service name", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("serviceDisplayName", "serviceDisplayName", moe::String,
                    "Windows service display name", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("serviceDescription", "serviceDescription", moe::String,
                    "Windows service description", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("serviceUser", "serviceUser", moe::String,
                    "account for service execution", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("servicePassword", "servicePassword", moe::String,
                    "password used to authenticate serviceUser", true));
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("service", "service", moe::Switch, "start mongodb service",
                    false));
        if (!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addSSLServerOptions(moe::OptionSection* options) {
        Status ret = options->addOption(OD("ssl.sslOnNormalPorts", "sslOnNormalPorts", moe::Switch,
                    "use ssl on configured ports", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ssl.PEMKeyFile", "sslPEMKeyFile", moe::String,
                    "PEM file for ssl" , true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ssl.PEMKeyPassword", "sslPEMKeyPassword", moe::String,
                    "PEM file password" , true, moe::Value(), moe::Value(std::string(""))));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ssl.clusterFile", "sslClusterFile", moe::String,
                    "Key file for internal SSL authentication" , true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ssl.clusterPassword", "sslClusterPassword", moe::String,
                    "Internal authentication key file password" , true, moe::Value(), moe::Value(std::string(""))));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ssl.CAFile", "sslCAFile", moe::String,
                    "Certificate Authority file for SSL", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ssl.CRLFile", "sslCRLFile", moe::String,
                    "Certificate Revocation List file for SSL", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ssl.weakCertificateValidation", "sslWeakCertificateValidation",
                    moe::Switch, "allow client to connect without presenting a certificate", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ssl.FIPSMode", "sslFIPSMode", moe::Switch,
                    "activate FIPS 140-2 mode at startup", true));
        if (!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

} // namespace mongo
