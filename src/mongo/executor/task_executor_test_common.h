// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <string>

namespace [[MONGO_MOD_PRIVATE]] mongo {
namespace executor {

class NetworkInterfaceMock;
class TaskExecutor;

/**
 * Sets up a unit test suite named "suiteName" that runs a battery of unit tests against executors
 * returned by "makeExecutor".  These tests should work against any implementation of TaskExecutor.
 *
 * The type of makeExecutor is a function that takes *a pointer to a unique_ptr* because of a
 * shortcoming in boost::function, that it does not know how process movable but not copyable
 * arguments in some circumstances. When we've switched to std::function on all platforms,
 * presumably after the release of MSVC2015, the signature can be changed to take the unique_ptr
 * by value.
 */
void addTestsForExecutor(
    const std::string& suiteName,
    std::function<std::shared_ptr<TaskExecutor>(std::unique_ptr<NetworkInterfaceMock>)>
        makeExecutor);

}  // namespace executor
}  // namespace mongo
