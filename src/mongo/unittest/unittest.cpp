/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
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

}  // namespace mongo::unittest
