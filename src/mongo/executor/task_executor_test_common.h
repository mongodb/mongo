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

#pragma once

#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <string>

namespace MONGO_MOD_PRIVATE mongo {
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
}  // namespace MONGO_MOD_PRIVATE mongo
