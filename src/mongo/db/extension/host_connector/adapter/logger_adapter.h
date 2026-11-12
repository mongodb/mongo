// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::extension::host_connector {

/**
 * The logging implementations, shared by every negotiated capability version.
 *
 * Neither MongoExtensionLoggerVTable entry takes a 'self' pointer, so the negotiated capability
 * reaches them through the identity of the function instead -- see LoggerAdapter below. The logic
 * lives here, outside the template, so only a trivial trampoline per entry is duplicated per
 * version.
 */
namespace detail {
::MongoExtensionStatus* log(const ::MongoExtensionLogMessage* logMessage,
                            const CapabilityVersion& capability);

::MongoExtensionStatus* shouldLog(::MongoExtensionLogSeverity levelOrSeverity,
                                  ::MongoExtensionLogType logType,
                                  bool* out,
                                  const CapabilityVersion& capability);
}  // namespace detail

template <uint32_t kPacked>
class LoggerAdapter;

/**
 * getLogger returns the LoggerAdapter instance for the given negotiated capability version. This
 * trampoline is the only way to obtain one: the per-version instances cannot be vended without a
 * negotiated capability.
 */
inline ::MongoExtensionLogger* getLogger(const CapabilityVersion& capability) {
    return getNegotiatedVersionInstance<LoggerAdapter, ::MongoExtensionLogger>(capability);
}

/**
 * Provides logging services for extensions.
 *
 * LoggerAdapter implements the MongoExtensionLogger interface to enable extensions to send
 * structured log messages to MongoDB's logv2 logging system. It acts as an adapter between
 * the C extension API and MongoDB's internal logging infrastructure.
 *
 * All extension log messages are routed to the EXTENSION-MONGOT log component.
 *
 * We use one instance per negotiable capability version. The version is baked into the type (and
 * exposed via kCapability) rather than stored in a member because neither vtable entry receives a
 * 'self' pointer, so a member would be unreachable from the very functions that need it. Each
 * instantiation gets its own VTABLE whose entries point at that instantiation's statics, which is
 * what makes kCapability recoverable inside them.
 *
 * The adapter is otherwise stateless; instances are shared by every extension that negotiated the
 * same version.
 *
 * kPacked must be a valid CapabilityVersion packed value representation.
 */
template <uint32_t kPacked>
class LoggerAdapter final : public ::MongoExtensionLogger {
public:
    static constexpr CapabilityVersion kCapability = CapabilityVersion::fromPackedValue(kPacked);

    LoggerAdapter(const LoggerAdapter&) = delete;
    LoggerAdapter& operator=(const LoggerAdapter&) = delete;
    LoggerAdapter(LoggerAdapter&&) = delete;
    LoggerAdapter& operator=(LoggerAdapter&&) = delete;

    static ::MongoExtensionLoggerVTable getVTable() {
        return VTABLE;
    }

private:
    template <template <uint32_t> class InstanceT, typename CApiT>
    friend CApiT* mongo::extension::getNegotiatedVersionInstance(const CapabilityVersion&);

    LoggerAdapter() : ::MongoExtensionLogger{&VTABLE} {}

    static LoggerAdapter& getInstance() {
        static LoggerAdapter sInstance;
        return sInstance;
    }

    static ::MongoExtensionLogger* getInstancePtr() {
        return &getInstance();
    }

    static ::MongoExtensionStatus* _extLog(const ::MongoExtensionLogMessage* logMessage) noexcept {
        return detail::log(logMessage, kCapability);
    }

    static ::MongoExtensionStatus* _extShouldLog(::MongoExtensionLogSeverity levelOrSeverity,
                                                 ::MongoExtensionLogType logType,
                                                 bool* out) noexcept {
        return detail::shouldLog(levelOrSeverity, logType, out, kCapability);
    }

    static constexpr ::MongoExtensionLoggerVTable VTABLE = {.log = &_extLog,
                                                            .should_log = &_extShouldLog};
};
}  // namespace mongo::extension::host_connector
