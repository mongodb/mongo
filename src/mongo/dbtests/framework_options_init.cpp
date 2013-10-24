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
 */

#include "mongo/dbtests/framework_options.h"

#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {
    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(FrameworkOptions)(InitializerContext* context) {
        return addTestFrameworkOptions(&moe::startupOptions);
    }

    MONGO_STARTUP_OPTIONS_VALIDATE(FrameworkOptions)(InitializerContext* context) {
        if (!handlePreValidationTestFrameworkOptions(moe::startupOptionsParsed, context->args())) {
            ::_exit(EXIT_SUCCESS);
        }
        Status ret = moe::startupOptionsParsed.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(FrameworkOptions)(InitializerContext* context) {
        Status ret = storeTestFrameworkOptions(moe::startupOptionsParsed, context->args());
        if (!ret.isOK()) {
            std::cerr << ret.toString() << std::endl;
            std::cerr << "try '" << context->args()[0] << " --help' for more information"
                      << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }
        return Status::OK();
    }
}
