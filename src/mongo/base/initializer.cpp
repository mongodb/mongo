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

#include "mongo/platform/basic.h"

#include "mongo/base/initializer.h"

#include <iostream>

#include "mongo/base/deinitializer_context.h"
#include "mongo/base/global_initializer.h"
#include "mongo/base/initializer_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/str.h"

namespace mongo {

Status Initializer::executeInitializers(const std::vector<std::string>& args) {
    auto oldState = std::exchange(_lifecycleState, State::kInitializing);
    invariant(oldState == State::kUninitialized, "invalid initializer state transition");

    if (_sortedNodes.empty()) {
        if (Status status = _graph.topSort(&_sortedNodes); !status.isOK()) {
            return status;
        }
    }
    _graph.freeze();

    InitializerContext context(args);

    for (const auto& nodeName : _sortedNodes) {
        InitializerDependencyNode* node = _graph.getInitializerNode(nodeName);

        // If already initialized then this node is a legacy initializer without re-initialization
        // support.
        if (node->isInitialized())
            continue;

        auto const& fn = node->getInitializerFunction();
        if (!fn) {
            return Status(ErrorCodes::InternalError,
                          "topSort returned a node that has no associated function: \"" + nodeName +
                              '"');
        }
        try {
            if (Status status = fn(&context); !status.isOK()) {
                return status;
            }
        } catch (const DBException& xcp) {
            return xcp.toStatus();
        }

        node->setInitialized(true);
    }

    oldState = std::exchange(_lifecycleState, State::kInitialized);
    invariant(oldState == State::kInitializing, "invalid initializer state transition");

    return Status::OK();
}

Status Initializer::executeDeinitializers() {
    auto oldState = std::exchange(_lifecycleState, State::kDeinitializing);
    invariant(oldState == State::kInitialized, "invalid initializer state transition");

    DeinitializerContext context{};

    // Execute deinitialization in reverse order from initialization.
    for (auto it = _sortedNodes.rbegin(), end = _sortedNodes.rend(); it != end; ++it) {
        InitializerDependencyNode* node = _graph.getInitializerNode(*it);
        auto const& fn = node->getDeinitializerFunction();
        if (fn) {
            try {
                if (Status status = fn(&context); !status.isOK()) {
                    return status;
                }
            } catch (const DBException& xcp) {
                return xcp.toStatus();
            }

            node->setInitialized(false);
        }
    }

    oldState = std::exchange(_lifecycleState, State::kUninitialized);
    invariant(oldState == State::kDeinitializing, "invalid initializer state transition");

    return Status::OK();
}

Status runGlobalInitializers(const std::vector<std::string>& argv) {
    return getGlobalInitializer().executeInitializers(argv);
}

Status runGlobalDeinitializers() {
    return getGlobalInitializer().executeDeinitializers();
}

void runGlobalInitializersOrDie(const std::vector<std::string>& argv) {
    Status status = runGlobalInitializers(argv);
    if (!status.isOK()) {
        std::cerr << "Failed global initialization: " << status << std::endl;
        quickExit(1);
    }
}

}  // namespace mongo
