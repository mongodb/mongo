/*
 *    Copyright (C) 2010 10gen Inc.
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
 */

#include "mongo/tools/mongofiles_options.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

namespace mongo {

    MongoFilesGlobalParams mongoFilesGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addMongoFilesOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("local", "local,l", moe::String,
                    "local filename for put|get (default is to use the same name as "
                    "'gridfs filename')", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("type", "type,t", moe::String,
                    "MIME type for put (default is to omit)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("replace", "replace,r", moe::Switch,
                    "Remove other files with same name after PUT", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "command", moe::String, 1 ));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addPositionalOption(POD( "file", moe::String, 2 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoFilesHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Browse and modify a GridFS filesystem.\n" << std::endl;
        *out << "usage: mongofiles [options] command [gridfs filename]" << std::endl;
        *out << "command:" << std::endl;
        *out << "  one of (list|search|put|get)" << std::endl;
        *out << "  list - list all files.  'gridfs filename' is an optional prefix " << std::endl;
        *out << "         which listed filenames must begin with." << std::endl;
        *out << "  search - search all files. 'gridfs filename' is a substring " << std::endl;
        *out << "           which listed filenames must contain." << std::endl;
        *out << "  put - add a file with filename 'gridfs filename'" << std::endl;
        *out << "  get - get a file with filename 'gridfs filename'" << std::endl;
        *out << "  delete - delete all files with filename 'gridfs filename'" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    Status handlePreValidationMongoFilesOptions(const moe::Environment& params) {
        if (toolsParsedOptions.count("help")) {
            printMongoFilesHelp(toolsOptions, &std::cout);
            ::_exit(0);
        }
        return Status::OK();
    }

    Status storeMongoFilesOptions(const moe::Environment& params,
                                  const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoFilesGlobalParams.command = getParam("command");
        mongoFilesGlobalParams.gridFSFilename = getParam("file");
        mongoFilesGlobalParams.localFile = getParam("local", mongoFilesGlobalParams.gridFSFilename);
        mongoFilesGlobalParams.contentType = getParam("type", "");
        mongoFilesGlobalParams.replace = hasParam("replace");

        return Status::OK();
    }

    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
            MONGO_NO_PREREQUISITES,
            ("default"))(InitializerContext* context) {

        toolsOptions = moe::OptionSection( "options" );
        moe::OptionsParser parser;

        Status retStatus = addMongoFilesOptions(&toolsOptions);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = parser.run(toolsOptions, context->args(), context->env(), &toolsParsedOptions);
        if (!retStatus.isOK()) {
            std::cerr << retStatus.reason() << std::endl;
            std::cerr << "try '" << context->args()[0]
                      << " --help' for more information" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }

        retStatus = handlePreValidationMongoFilesOptions(toolsParsedOptions);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = toolsParsedOptions.validate();
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = storeMongoFilesOptions(toolsParsedOptions, context->args());
        if (!retStatus.isOK()) {
            return retStatus;
        }

        return Status::OK();
    }
}
