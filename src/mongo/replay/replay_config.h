// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <chrono>
#include <string>
#include <vector>

namespace mongo {
struct ReplayConfig {
    std::string recordingPath;
    std::string mongoURI;
    std::string enablePerformanceRecording;
    std::chrono::seconds sessionPreInitTime = std::chrono::seconds(5);

    explicit operator bool() const {
        return !recordingPath.empty() && !mongoURI.empty();
    }
};

inline BSONObj toBSON(const ReplayConfig& cfg) {
    BSONObjBuilder bob;
    bob.append("recordingPath", cfg.recordingPath);
    bob.append("mongoURI", cfg.mongoURI);
    bob.append("enablePerformanceRecording", cfg.enablePerformanceRecording);
    bob.append("sessionPreInitTime", cfg.sessionPreInitTime.count());
    return bob.obj();
}

using ReplayConfigs = std::vector<ReplayConfig>;
}  // namespace mongo
