/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kControl

#include "mongo/logv2/log_util.h"

#include "mongo/logger/logger.h"
#include "mongo/logger/rotatable_file_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/util/time_support.h"

#include <string>

namespace mongo::logv2 {
bool rotateLogs(bool renameFiles) {
    // Rotate on both logv1 and logv2 so all files that need rotation gets rotated
    LOGV2(23166, "Log rotation initiated");
    std::string suffix = "." + terseCurrentTime(false);
    Status resultv2 =
        logv2::LogManager::global().getGlobalDomainInternal().rotate(renameFiles, suffix);
    if (!resultv2.isOK())
        LOGV2_WARNING(23168, "Log rotation failed", "reason"_attr = resultv2);

    using logger::RotatableFileManager;
    RotatableFileManager* manager = logger::globalRotatableFileManager();
    RotatableFileManager::FileNameStatusPairVector result(manager->rotateAll(renameFiles, suffix));
    for (RotatableFileManager::FileNameStatusPairVector::iterator it = result.begin();
         it != result.end();
         it++) {
        LOGV2_WARNING(
            23169, "Rotating log file failed", "file"_attr = it->first, "reason"_attr = it->second);
    }
    return resultv2.isOK() && result.empty();
}
}  // namespace mongo::logv2
