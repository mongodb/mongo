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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "bongo/s/bongos_options.h"

#include <iostream>

#include "bongo/util/exit_code.h"
#include "bongo/util/options_parser/startup_option_init.h"
#include "bongo/util/options_parser/startup_options.h"
#include "bongo/util/quick_exit.h"

namespace bongo {
BONGO_GENERAL_STARTUP_OPTIONS_REGISTER(BongosOptions)(InitializerContext* context) {
    return addBongosOptions(&moe::startupOptions);
}

BONGO_INITIALIZER_GENERAL(BongosOptions,
                          ("BeginStartupOptionValidation", "AllFailPointsRegistered"),
                          ("EndStartupOptionValidation"))
(InitializerContext* context) {
    if (!handlePreValidationBongosOptions(moe::startupOptionsParsed, context->args())) {
        quickExit(EXIT_SUCCESS);
    }
    // Run validation, but tell the Environment that we don't want it to be set as "valid",
    // since we may be making it invalid in the canonicalization process.
    Status ret = moe::startupOptionsParsed.validate(false /*setValid*/);
    if (!ret.isOK()) {
        return ret;
    }
    ret = validateBongosOptions(moe::startupOptionsParsed);
    if (!ret.isOK()) {
        return ret;
    }
    ret = canonicalizeBongosOptions(&moe::startupOptionsParsed);
    if (!ret.isOK()) {
        return ret;
    }
    ret = moe::startupOptionsParsed.validate();
    if (!ret.isOK()) {
        return ret;
    }
    return Status::OK();
}

BONGO_INITIALIZER_GENERAL(BongosOptions_Store,
                          ("BeginStartupOptionStorage"),
                          ("EndStartupOptionStorage"))
(InitializerContext* context) {
    Status ret = storeBongosOptions(moe::startupOptionsParsed);
    if (!ret.isOK()) {
        std::cerr << ret.toString() << std::endl;
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        quickExit(EXIT_BADOPTIONS);
    }
    return Status::OK();
}

}  // namespace bongo
