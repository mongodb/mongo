/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/base/initializer.h"

#include "mongo/base/global_initializer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/quick_exit.h"
#include <iostream>

namespace mongo {

Initializer::Initializer() {}
Initializer::~Initializer() {}

Status Initializer::execute(const InitializerContext::ArgumentVector& args,
                            const InitializerContext::EnvironmentMap& env) const {
    std::vector<std::string> sortedNodes;
    Status status = _graph.topSort(&sortedNodes);
    if (Status::OK() != status)
        return status;

    InitializerContext context(args, env);

    for (size_t i = 0; i < sortedNodes.size(); ++i) {
        InitializerFunction fn = _graph.getInitializerFunction(sortedNodes[i]);
        if (!fn) {
            return Status(ErrorCodes::InternalError,
                          "topSort returned a node that has no associated function: \"" +
                              sortedNodes[i] + '"');
        }
        try {
            status = fn(&context);
        } catch (const DBException& xcp) {
            return xcp.toStatus();
        }

        if (Status::OK() != status)
            return status;
    }
    return Status::OK();
}

Status runGlobalInitializers(const InitializerContext::ArgumentVector& args,
                             const InitializerContext::EnvironmentMap& env) {
    return getGlobalInitializer().execute(args, env);
}

Status runGlobalInitializers(int argc, const char* const* argv, const char* const* envp) {
    InitializerContext::ArgumentVector args(argc);
    std::copy(argv, argv + argc, args.begin());

    InitializerContext::EnvironmentMap env;

    if (envp) {
        for (; *envp; ++envp) {
            const char* firstEqualSign = strchr(*envp, '=');
            if (!firstEqualSign) {
                return Status(ErrorCodes::BadValue, "malformed environment block");
            }
            env[std::string(*envp, firstEqualSign)] = std::string(firstEqualSign + 1);
        }
    }

    return runGlobalInitializers(args, env);
}

void runGlobalInitializersOrDie(int argc, const char* const* argv, const char* const* envp) {
    Status status = runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        std::cerr << "Failed global initialization: " << status << std::endl;
        quickExit(1);
    }
}

}  // namespace mongo
