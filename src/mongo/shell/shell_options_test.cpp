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

#include "mongo/shell/shell_options.h"

#include "mongo/bson/util/builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_options.h"

namespace {

    namespace moe = ::mongo::optionenvironment;

    TEST(Registration, RegisterAllOptions) {

        moe::OptionSection options;

        ASSERT_OK(::mongo::addMongoShellOptions(&options));

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(options.getAllOptions(&options_vector));

        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "shell") {
                ASSERT_EQUALS(iterator->_singleName, "shell");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "run the shell after executing files");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "nodb") {
                ASSERT_EQUALS(iterator->_singleName, "nodb");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "don't connect to mongod on startup - no 'db address' arg expected");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "norc") {
                ASSERT_EQUALS(iterator->_singleName, "norc");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "will not run the \".mongorc.js\" file on start up");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "quiet") {
                ASSERT_EQUALS(iterator->_singleName, "quiet");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "be less chatty");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "port") {
                ASSERT_EQUALS(iterator->_singleName, "port");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "port to connect to");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "host") {
                ASSERT_EQUALS(iterator->_singleName, "host");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "server to connect to");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "eval") {
                ASSERT_EQUALS(iterator->_singleName, "eval");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "evaluate javascript");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "username") {
                ASSERT_EQUALS(iterator->_singleName, "username,u");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "username for authentication");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "password") {
                ASSERT_EQUALS(iterator->_singleName, "password,p");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "password for authentication");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                moe::Value implicitVal(std::string(""));
                ASSERT_TRUE(iterator->_implicit.equal(implicitVal));
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "authenticationDatabase") {
                ASSERT_EQUALS(iterator->_singleName, "authenticationDatabase");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "user source (defaults to dbname)");
                ASSERT_EQUALS(iterator->_isVisible, true);
                moe::Value defaultVal(std::string(""));
                ASSERT_TRUE(iterator->_default.equal(defaultVal));
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "authenticationMechanism") {
                ASSERT_EQUALS(iterator->_singleName, "authenticationMechanism");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "authentication mechanism");
                ASSERT_EQUALS(iterator->_isVisible, true);
                moe::Value defaultVal(std::string("MONGODB-CR"));
                ASSERT_TRUE(iterator->_default.equal(defaultVal));
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
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
            }
            else if (iterator->_dottedName == "verbose") {
                ASSERT_EQUALS(iterator->_singleName, "verbose");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "increase verbosity");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
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
            }
            else if (iterator->_dottedName == "dbaddress") {
                ASSERT_EQUALS(iterator->_singleName, "dbaddress");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "dbaddress");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "files") {
                ASSERT_EQUALS(iterator->_singleName, "files");
                ASSERT_EQUALS(iterator->_type, moe::StringVector);
                ASSERT_EQUALS(iterator->_description, "files");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "nokillop") {
                ASSERT_EQUALS(iterator->_singleName, "nokillop");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "nokillop");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "autokillop") {
                ASSERT_EQUALS(iterator->_singleName, "autokillop");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "autokillop");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
#ifdef MONGO_SSL
            else if (iterator->_dottedName == "ssl") {
                ASSERT_EQUALS(iterator->_singleName, "ssl");
                ASSERT_EQUALS(iterator->_type, moe::Switch);
                ASSERT_EQUALS(iterator->_description, "use SSL for all connections");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
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
            }
            else if (iterator->_dottedName == "ssl.PEMKeyFile") {
                ASSERT_EQUALS(iterator->_singleName, "sslPEMKeyFile");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "PEM certificate/key file for SSL");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
            }
            else if (iterator->_dottedName == "ssl.PEMKeyPassword") {
                ASSERT_EQUALS(iterator->_singleName, "sslPEMKeyPassword");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "password for key in PEM file for SSL");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
                ASSERT_EQUALS(iterator->_sources, moe::SourceAll);
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
