// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/util/modules.h"

#define MAKE_EXCEPTION_INFO(code, message) \
    BSON("message" << message << "errorCode" << static_cast<int>(code));

#define sdk_uasserted(code, message)                                                    \
    do {                                                                                \
        auto exceptionInfo = MAKE_EXCEPTION_INFO(code, message);                        \
        mongo::extension::invokeCAndConvertStatusToException([&]() {                    \
            return mongo::extension::sdk::HostServicesAPI::getInstance()->userAsserted( \
                mongo::extension::objAsByteView(exceptionInfo));                        \
        });                                                                             \
    } while (false)

#define sdk_tasserted(code, message)                                                        \
    do {                                                                                    \
        auto exceptionInfo = MAKE_EXCEPTION_INFO(code, message);                            \
        mongo::extension::invokeCAndConvertStatusToException([&]() {                        \
            return mongo::extension::sdk::HostServicesAPI::getInstance()->tripwireAsserted( \
                mongo::extension::objAsByteView(exceptionInfo));                            \
        });                                                                                 \
    } while (false)

#define sdk_uassert(code, message, condition) \
    do {                                      \
        if (MONGO_unlikely(!(condition))) {   \
            sdk_uasserted(code, message);     \
        }                                     \
    } while (false)

#define sdk_tassert(code, message, condition) \
    do {                                      \
        if (MONGO_unlikely(!(condition))) {   \
            sdk_tasserted(code, message);     \
        }                                     \
    } while (false)
