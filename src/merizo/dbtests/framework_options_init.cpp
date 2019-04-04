/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "merizo/platform/basic.h"

#include <iostream>

#include "merizo/dbtests/framework_options.h"
#include "merizo/util/exit_code.h"
#include "merizo/util/options_parser/startup_option_init.h"
#include "merizo/util/options_parser/startup_options.h"
#include "merizo/util/quick_exit.h"

namespace merizo {
MERIZO_GENERAL_STARTUP_OPTIONS_REGISTER(FrameworkOptions)(InitializerContext* context) {
    return addTestFrameworkOptions(&moe::startupOptions);
}

MERIZO_STARTUP_OPTIONS_VALIDATE(FrameworkOptions)(InitializerContext* context) {
    if (!handlePreValidationTestFrameworkOptions(moe::startupOptionsParsed, context->args())) {
        quickExit(EXIT_SUCCESS);
    }
    Status ret = moe::startupOptionsParsed.validate();
    if (!ret.isOK()) {
        return ret;
    }
    return Status::OK();
}

MERIZO_STARTUP_OPTIONS_STORE(FrameworkOptions)(InitializerContext* context) {
    Status ret = storeTestFrameworkOptions(moe::startupOptionsParsed, context->args());
    if (!ret.isOK()) {
        std::cerr << ret.toString() << std::endl;
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        quickExit(EXIT_BADOPTIONS);
    }
    return Status::OK();
}

MERIZO_INITIALIZER_GENERAL(CoreOptions_Store, MERIZO_NO_PREREQUISITES, MERIZO_NO_DEPENDENTS)
(InitializerContext* context) {
    return Status::OK();
}
}
