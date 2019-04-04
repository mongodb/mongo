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
#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kFTDC

#include "merizo/platform/basic.h"

#include "merizo/db/ftdc/ftdc_merizos.h"

#include <boost/filesystem.hpp>

#include "merizo/db/ftdc/controller.h"
#include "merizo/db/ftdc/ftdc_server.h"
#include "merizo/stdx/thread.h"
#include "merizo/util/log.h"
#include "merizo/util/synchronized_value.h"

namespace merizo {

void registerMerizoSCollectors(FTDCController* controller) {
    // PoolStats
    controller->addPeriodicCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "connPoolStats", "connPoolStats", "", BSON("connPoolStats" << 1)));
}

void startMerizoSFTDC() {
    // Get the path to use for FTDC:
    // 1. Check if the user set one.
    // 2. If not, check if the user has a logpath and derive one.
    // 3. Otherwise, tell the user FTDC cannot run.

    // Only attempt to enable FTDC if we have a path to log files to.
    FTDCStartMode startMode = FTDCStartMode::kStart;
    auto directory = getFTDCDirectoryPathParameter();

    if (directory.empty()) {
        if (serverGlobalParams.logpath.empty()) {
            warning() << "FTDC is disabled because neither '--logpath' nor set parameter "
                         "'diagnosticDataCollectionDirectoryPath' are specified.";
            startMode = FTDCStartMode::kSkipStart;
        } else {
            directory = boost::filesystem::absolute(
                FTDCUtil::getMerizoSPath(serverGlobalParams.logpath), serverGlobalParams.cwd);

            // Note: If the computed FTDC directory conflicts with an existing file, then FTDC will
            // warn about the conflict, and not startup. It will not terminate MerizoS in this
            // situation.
        }
    }

    startFTDC(directory, startMode, registerMerizoSCollectors);
}

void stopMerizoSFTDC() {
    stopFTDC();
}

}  // namespace merizo
