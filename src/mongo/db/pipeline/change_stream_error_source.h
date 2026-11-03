// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

// Category of a converted change stream error. Reported as 'error.source' in operationType:"error"
// events and tracked under
// changeStreams.error.converted.{userPipeline,internalPipeline,resumeGap,oversizedEvent}.
enum class ChangeStreamErrorSource {
    kUserPipeline,
    kInternalPipeline,
    kResumeGap,
    kOversizedEvent,
};

// Wire values of 'error.source'.
constexpr std::string_view kCsUserPipelineSource{"user-pipeline"};
constexpr std::string_view kCsInternalPipelineSource{"internal-pipeline"};
constexpr std::string_view kCsResumeGapSource{"resume-gap"};
constexpr std::string_view kCsOversizedEventSource{"oversized-event"};

constexpr std::string_view toString(ChangeStreamErrorSource source) {
    switch (source) {
        case ChangeStreamErrorSource::kUserPipeline:
            return kCsUserPipelineSource;
        case ChangeStreamErrorSource::kInternalPipeline:
            return kCsInternalPipelineSource;
        case ChangeStreamErrorSource::kResumeGap:
            return kCsResumeGapSource;
        case ChangeStreamErrorSource::kOversizedEvent:
            return kCsOversizedEventSource;
    }
    return kCsUserPipelineSource;
}

}  // namespace mongo
