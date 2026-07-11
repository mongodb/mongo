// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/exception/exception.hpp>
#include <boost/log/core/core.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>
#include <fmt/printf.h>  // IWYU pragma: keep
// IWYU pragma: no_include "boost/log/detail/attachable_sstream_buf.hpp"
// IWYU pragma: no_include "boost/log/detail/locking_ptr.hpp"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/domain_filter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"
#include "mongo/util/version/releases.h"

#include <boost/log/sinks/unlocked_frontend.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/thread/exceptions.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::unittest {

SpawnInfo& getSpawnInfo() {
    static auto v = new SpawnInfo{};
    return *v;
}

AutoUpdateConfig& getAutoUpdateConfig() {
    static AutoUpdateConfig config{};
    return config;
}

MockBehavior getDefaultMockBehavior() {
    return static_cast<MockBehavior>(GMOCK_FLAG_GET(default_mock_behavior));
}

void setDefaultMockBehavior(MockBehavior behavior) {
    GMOCK_FLAG_SET(default_mock_behavior, static_cast<int>(behavior));
}

}  // namespace mongo::unittest
