/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/service_context_registrar.h"

#include "mongo/base/init.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {
std::function<std::unique_ptr<ServiceContext>()>& getServiceContextFactory() {
    static std::function<std::unique_ptr<ServiceContext>()> factory;
    return factory;
}

// clang-format off
GlobalInitializerRegisterer registerCreateServiceContext{
    "ServiceContext",
    {"default"},
    [](InitializerContext* context) {
        // Set the global service context if a service context factory was previously registered.
        if (getServiceContextFactory()) {
            setGlobalServiceContext(getServiceContextFactory()());
        }
        return Status::OK();
    },
    [](DeinitializerContext* context) {
        // For now, deregistration is done manually after all deinitializers run, in case any
        // erroneously access the globalServiceContext without expressing a dependency.
        return Status::OK();
    }
};
// clang-format on

}  // namespace

ServiceContextRegistrar::ServiceContextRegistrar(
    std::function<std::unique_ptr<ServiceContext>()> fn) {
    invariant(!getServiceContextFactory());
    getServiceContextFactory() = std::move(fn);
}

}  // namespace mongo
