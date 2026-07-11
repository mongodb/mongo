// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/throw_hook_options_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/throw_hook.h"
#include "mongo/util/stacktrace.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {

auto doInit = [] {
    setThrowHook([](std::type_info* tinfo, void* ex) {
        if (gThrowLoggingEnabled.loadRelaxed() &&
            (gThrowLoggingExceptionName == tinfo->name() || !catchCast<DBException>(tinfo, ex))) {
            LOGV2_WARNING(9891800, "Threw exception", "type"_attr = tinfo->name());
            printStackTrace();
        }
    });
    return 0;
}();

}  // namespace

}  // namespace mongo
