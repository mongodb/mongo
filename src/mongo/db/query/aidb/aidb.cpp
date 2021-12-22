/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/aidb/ai_application.h"
#include "mongo/db/query/aidb/ai_command.h"
#include "mongo/db/query/aidb/ai_data_generator.h"
#include "mongo/db/query/aidb/ai_database.h"
#include "mongo/db/query/aidb/ai_index_accessor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/util/invariant.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/testing_proctor.h"

namespace mongo {

void run() {
    ai::Application app{};
    app.run();
}

void configureLogging(const std::string logPath) {
    auto& lv2Manager = mongo::logv2::LogManager::global();
    mongo::logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.consoleEnabled = false;
    lv2Config.fileEnabled = true;
    lv2Config.filePath = logPath;
    lv2Config.fileRotationMode =
        mongo::logv2::LogDomainGlobal::ConfigurationOptions::RotationMode::kReopen;
    lv2Config.fileOpenMode =
        mongo::logv2::LogDomainGlobal::ConfigurationOptions::OpenMode::kTruncate;
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}

}  // namespace mongo

int main(int argc, char** argv) {
    ::mongo::configureLogging("aidb.log");

    ::mongo::clearSignalMask();
    ::mongo::setupSynchronousSignalHandlers();

    std::vector<std::string> argVec(argv, argv + argc);
    ::mongo::TestingProctor::instance().setEnabled(true);
    ::mongo::runGlobalInitializersOrDie(argVec);

    ::mongo::run();

    auto ret = ::mongo::runGlobalDeinitializers();
    if (!ret.isOK()) {
        std::cerr << "Global deinitilization failed: " << ret.reason() << std::endl;
    }

    ::mongo::TestingProctor::instance().exitAbruptlyIfDeferredErrors();

    return 0;
}
