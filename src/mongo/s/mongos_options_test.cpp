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

#include "mongo/s/mongos_options.h"

#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_options.h"

namespace {

    namespace moe = ::mongo::optionenvironment;

    TEST(Registration, RegisterAllOptions) {

        moe::OptionSection options;

        ASSERT_OK(::mongo::addMongosOptions(&options));

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(options.getAllOptions(&options_vector));

        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "noAutoSplit") {
                ASSERT_EQUALS(iterator->_singleName, "noAutoSplit");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "do not send split commands with writes");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "help") {
                ASSERT_EQUALS(iterator->_singleName, "help,h");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "show this usage information");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "version") {
                ASSERT_EQUALS(iterator->_singleName, "version");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "show version information");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "config") {
                ASSERT_EQUALS(iterator->_singleName, "config,f");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "configuration file specifying additional options");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "verbose") {
                ASSERT_EQUALS(iterator->_singleName, "verbose,v");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "be more verbose (include multiple times for more verbosity e.g. -vvvvv)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                moe::Value implicitVal(std::string("v"));
                ASSERT_TRUE(iterator->_implicit.equal(implicitVal));
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "quiet") {
                ASSERT_EQUALS(iterator->_singleName, "quiet");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "quieter output");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "port") {
                ASSERT_EQUALS(iterator->_singleName, "port");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "specify port number - 27017 by default");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "bind_ip") {
                ASSERT_EQUALS(iterator->_singleName, "bind_ip");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "comma separated list of ip addresses to listen on - all local ips by default");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "maxConns") {
                ASSERT_EQUALS(iterator->_singleName, "maxConns");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "max number of simultaneous connections - 20000 by default");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "logpath") {
                ASSERT_EQUALS(iterator->_singleName, "logpath");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "log file to send write to instead of stdout - has to be a file, not directory");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "syslogFacility") {
                ASSERT_EQUALS(iterator->_singleName, "syslogFacility");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "syslog facility used for monogdb syslog message");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "logappend") {
                ASSERT_EQUALS(iterator->_singleName, "logappend");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "append to logpath instead of over-writing");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "logTimestampFormat") {
                ASSERT_EQUALS(iterator->_singleName, "logTimestampFormat");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Desired format for timestamps in log messages. One of ctime, iso8601-utc or iso8601-local");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "pidfilepath") {
                ASSERT_EQUALS(iterator->_singleName, "pidfilepath");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "full path to pidfile (if not set, no pidfile is created)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "keyFile") {
                ASSERT_EQUALS(iterator->_singleName, "keyFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "private key for cluster authentication");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "setParameter") {
                ASSERT_EQUALS(iterator->_singleName, "setParameter");
                ASSERT_EQUALS(iterator->_type, moe::StringVector);
                ASSERT_EQUALS(iterator->_description, "Set a configurable parameter");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, true);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "httpinterface") {
                ASSERT_EQUALS(iterator->_singleName, "httpinterface");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "enable http interface");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "clusterAuthMode") {
                ASSERT_EQUALS(iterator->_singleName, "clusterAuthMode");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Authentication mode used for cluster authentication. Alternatives are (keyFile|sendKeyFile|sendX509|x509)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "nounixsocket") {
                ASSERT_EQUALS(iterator->_singleName, "nounixsocket");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable listening on unix sockets");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "unixSocketPrefix") {
                ASSERT_EQUALS(iterator->_singleName, "unixSocketPrefix");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "alternative directory for UNIX domain sockets (defaults to /tmp)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "fork") {
                ASSERT_EQUALS(iterator->_singleName, "fork");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "fork server process");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "syslog") {
                ASSERT_EQUALS(iterator->_singleName, "syslog");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "log to system's syslog facility instead of file or stdout");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vv") {
                ASSERT_EQUALS(iterator->_singleName, "vv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvvvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvvvvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvvvvvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvvvvvvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvvvvvvvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "vvvvvvvvvvvv") {
                ASSERT_EQUALS(iterator->_singleName, "vvvvvvvvvvvv");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "verbose");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "nohttpinterface") {
                ASSERT_EQUALS(iterator->_singleName, "nohttpinterface");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable http interface");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "objcheck") {
                ASSERT_EQUALS(iterator->_singleName, "objcheck");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "inspect client data for validity on receipt (DEFAULT)");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "noobjcheck") {
                ASSERT_EQUALS(iterator->_singleName, "noobjcheck");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "do NOT inspect client data for validity on receipt");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "traceExceptions") {
                ASSERT_EQUALS(iterator->_singleName, "traceExceptions");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "log stack traces for every exception");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "enableExperimentalIndexStatsCmd") {
                ASSERT_EQUALS(iterator->_singleName, "enableExperimentalIndexStatsCmd");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "EXPERIMENTAL (UNSUPPORTED). Enable command computing aggregate statistics on indexes.");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "enableExperimentalStorageDetailsCmd") {
                ASSERT_EQUALS(iterator->_singleName, "enableExperimentalStorageDetailsCmd");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "EXPERIMENTAL (UNSUPPORTED). Enable command computing aggregate statistics on storage.");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "configdb") {
                ASSERT_EQUALS(iterator->_singleName, "configdb");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "1 or 3 comma separated config servers");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "localThreshold") {
                ASSERT_EQUALS(iterator->_singleName, "localThreshold");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "ping time (in ms) for a node to be considered local (default 15ms)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "test") {
                ASSERT_EQUALS(iterator->_singleName, "test");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "just run unit tests");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "upgrade") {
                ASSERT_EQUALS(iterator->_singleName, "upgrade");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "upgrade meta data version");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "chunkSize") {
                ASSERT_EQUALS(iterator->_singleName, "chunkSize");
                ASSERT_EQUALS(iterator->_type, moe::Int);
                ASSERT_EQUALS(iterator->_description, "maximum amount of data per chunk (in MB)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ipv6") {
                ASSERT_EQUALS(iterator->_singleName, "ipv6");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "enable IPv6 support (disabled by default)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "jsonp") {
                ASSERT_EQUALS(iterator->_singleName, "jsonp");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "allow JSONP access via http (has security implications)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "noscripting") {
                ASSERT_EQUALS(iterator->_singleName, "noscripting");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "disable scripting engine");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
#ifdef MONGO_SSL
            else if (iterator->_dottedName == "ssl.sslOnNormalPorts") {
                ASSERT_EQUALS(iterator->_singleName, "sslOnNormalPorts");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "use ssl on configured ports");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.mode") {
                ASSERT_EQUALS(iterator->_singleName, "sslMode");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "set the SSL operation mode (disabled|allowSSL|preferSSL|requireSSL)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.PEMKeyFile") {
                ASSERT_EQUALS(iterator->_singleName, "sslPEMKeyFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "PEM file for ssl");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.PEMKeyPassword") {
                ASSERT_EQUALS(iterator->_singleName, "sslPEMKeyPassword");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "PEM file password");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                moe::Value implicitVal(std::string(""));
                ASSERT_TRUE(iterator->_implicit.equal(implicitVal));
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.clusterFile") {
                ASSERT_EQUALS(iterator->_singleName, "sslClusterFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Key file for internal SSL authentication");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.clusterPassword") {
                ASSERT_EQUALS(iterator->_singleName, "sslClusterPassword");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Internal authentication key file password");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                moe::Value implicitVal(std::string(""));
                ASSERT_TRUE(iterator->_implicit.equal(implicitVal));
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.CAFile") {
                ASSERT_EQUALS(iterator->_singleName, "sslCAFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Certificate Authority file for SSL");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.CRLFile") {
                ASSERT_EQUALS(iterator->_singleName, "sslCRLFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Certificate Revocation List file for SSL");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.weakCertificateValidation") {
                ASSERT_EQUALS(iterator->_singleName, "sslWeakCertificateValidation");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "allow client to connect without presenting a certificate");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.allowInvalidCertificates") {
                ASSERT_EQUALS(iterator->_singleName, "sslAllowInvalidCertificates");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "allow connections to servers with invalid certificates");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "ssl.FIPSMode") {
                ASSERT_EQUALS(iterator->_singleName, "sslFIPSMode");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "activate FIPS 140-2 mode at startup");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
#endif
#ifdef _WIN32
            else if (iterator->_dottedName == "install") {
                ASSERT_EQUALS(iterator->_singleName, "install");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "install Windows service");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "remove") {
                ASSERT_EQUALS(iterator->_singleName, "remove");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "remove Windows service");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "reinstall") {
                ASSERT_EQUALS(iterator->_singleName, "reinstall");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "reinstall Windows service (equivalent to --remove followed by --install)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "serviceName") {
                ASSERT_EQUALS(iterator->_singleName, "serviceName");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Windows service name");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "serviceDisplayName") {
                ASSERT_EQUALS(iterator->_singleName, "serviceDisplayName");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Windows service display name");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "serviceDescription") {
                ASSERT_EQUALS(iterator->_singleName, "serviceDescription");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Windows service description");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "serviceUser") {
                ASSERT_EQUALS(iterator->_singleName, "serviceUser");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "account for service execution");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "servicePassword") {
                ASSERT_EQUALS(iterator->_singleName, "servicePassword");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "password used to authenticate serviceUser");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
            else if (iterator->_dottedName == "service") {
                ASSERT_EQUALS(iterator->_singleName, "service");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "start mongodb service");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
                ASSERT_EQUALS(iterator->_positionalStart, -1);
                ASSERT_EQUALS(iterator->_positionalEnd, -1);
            }
#endif
            else {
                ::mongo::StringBuilder sb;
                sb << "Found extra option: " << iterator->_dottedName <<
                      " which we did not register";
                FAIL(sb.str());
            }
        }
    }

} // namespace
