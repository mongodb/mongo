/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"

#include <boost/filesystem/path.hpp>

#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/data_protector.h"
#include "mongo/stdx/memory.h"

namespace mongo {

/* Make a WiredTigerCustomizationHooks pointer a decoration on the global ServiceContext */
MONGO_INITIALIZER_WITH_PREREQUISITES(SetWiredTigerCustomizationHooks, ("SetGlobalEnvironment"))
(InitializerContext* context) {
    auto customizationHooks = stdx::make_unique<EmptyWiredTigerCustomizationHooks>();
    WiredTigerCustomizationHooks::set(getGlobalServiceContext(), std::move(customizationHooks));

    return Status::OK();
}

namespace {
const auto getCustomizationHooks =
    ServiceContext::declareDecoration<std::unique_ptr<WiredTigerCustomizationHooks>>();
}  // namespace

void WiredTigerCustomizationHooks::set(ServiceContext* service,
                                       std::unique_ptr<WiredTigerCustomizationHooks> custHooks) {
    auto& hooks = getCustomizationHooks(service);
    invariant(custHooks);
    hooks = std::move(custHooks);
}

WiredTigerCustomizationHooks* WiredTigerCustomizationHooks::get(ServiceContext* service) {
    return getCustomizationHooks(service).get();
}

EmptyWiredTigerCustomizationHooks::~EmptyWiredTigerCustomizationHooks() {}

bool EmptyWiredTigerCustomizationHooks::enabled() const {
    return false;
}

bool EmptyWiredTigerCustomizationHooks::restartRequired() {
    return false;
}

std::string EmptyWiredTigerCustomizationHooks::getOpenConfig(StringData tableName) {
    return "";
}


std::unique_ptr<DataProtector> EmptyWiredTigerCustomizationHooks::getDataProtector() {
    return std::unique_ptr<DataProtector>();
}

boost::filesystem::path EmptyWiredTigerCustomizationHooks::getProtectedPathSuffix() {
    return "";
}

Status EmptyWiredTigerCustomizationHooks::protectTmpData(
    const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* resultLen) {
    return Status(ErrorCodes::InternalError,
                  "Customization hooks must be enabled to use preprocessTmpData.");
}

Status EmptyWiredTigerCustomizationHooks::unprotectTmpData(
    const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* resultLen) {
    return Status(ErrorCodes::InternalError,
                  "Customization hooks must be enabled to use postprocessTmpData.");
}
}  // namespace mongo
