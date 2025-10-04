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

#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

namespace mongo {

namespace {
const auto getConfigHooks = ServiceContext::declareDecoration<WiredTigerExtensions>();
const auto getSpillConfigHooks = ServiceContext::declareDecoration<SpillWiredTigerExtensions>();
}  // namespace

WiredTigerExtensions& WiredTigerExtensions::get(ServiceContext* service) {
    return getConfigHooks(service);
}

SpillWiredTigerExtensions& SpillWiredTigerExtensions::get(ServiceContext* service) {
    return getSpillConfigHooks(service);
}

std::string WiredTigerExtensions::getOpenExtensionsConfig() const {
    if (_wtExtensions.size() == 0) {
        return "";
    }

    StringBuilder extensions;
    extensions << "extensions=[";
    for (const auto& ext : _wtExtensions) {
        extensions << ext << ",";
    }
    extensions << "],";

    return extensions.str();
}

void WiredTigerExtensions::addExtension(StringData extensionConfigStr) {
    _wtExtensions.emplace_back(std::string{extensionConfigStr});
}

std::string SpillWiredTigerExtensions::getOpenExtensionsConfig() const {
    if (_wtExtensions.size() == 0) {
        return "";
    }

    StringBuilder extensions;
    extensions << "extensions=[";
    for (const auto& ext : _wtExtensions) {
        extensions << ext << ",";
    }
    extensions << "],";

    return extensions.str();
}

void SpillWiredTigerExtensions::addExtension(StringData extensionConfigStr) {
    _wtExtensions.emplace_back(std::string{extensionConfigStr});
}

}  // namespace mongo
