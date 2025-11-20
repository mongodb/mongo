/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/public/api.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo::extension::host_connector {

class HostPortalBase {
public:
    virtual ~HostPortalBase() = default;
    virtual void registerStageDescriptor(const ::MongoExtensionAggStageDescriptor*) const = 0;
};

class HostPortalAdapter final : public ::MongoExtensionHostPortal {
public:
    HostPortalAdapter(::MongoExtensionAPIVersion apiVersion,
                      int maxWireVersion,
                      std::string extensionOptions,
                      std::unique_ptr<HostPortalBase> portal)
        : ::MongoExtensionHostPortal{&VTABLE, apiVersion, maxWireVersion},
          _extensionOpts(std::move(extensionOptions)),
          _portal(std::move(portal)) {
        tassert(11417104, "Provided HostPortalBase is null", _portal != nullptr);
    }

    HostPortalAdapter(const HostPortalAdapter&) = delete;
    HostPortalAdapter& operator=(const HostPortalAdapter&) = delete;
    HostPortalAdapter(HostPortalAdapter&&) = delete;
    HostPortalAdapter& operator=(HostPortalAdapter&&) = delete;

    const HostPortalBase& getImpl() const {
        return *_portal;
    }

private:
    static ::MongoExtensionStatus* _extRegisterStageDescriptor(
        const MongoExtensionHostPortal* hostPortal,
        const MongoExtensionAggStageDescriptor* stageDesc) noexcept;

    static ::MongoExtensionByteView _extGetOptions(
        const ::MongoExtensionHostPortal* portal) noexcept;

    static constexpr ::MongoExtensionHostPortalVTable VTABLE{&_extRegisterStageDescriptor,
                                                             &_extGetOptions};

    const std::string _extensionOpts;
    std::unique_ptr<HostPortalBase> _portal;
};

}  // namespace mongo::extension::host_connector
