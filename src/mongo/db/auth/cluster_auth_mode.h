// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

class ServiceContext;

/**
 * ClusterAuthMode is a thin wrapper around an enum for decorated storage and semantic utility.
 */
class [[MONGO_MOD_PUBLIC]] ClusterAuthMode {
    enum class Value {
        kUndefined,
        /**
         * Authenticate using keyfile, accept only keyfiles
         */
        kKeyFile,

        /**
         * Authenticate using keyfile, accept both keyfiles and X.509
         */
        kSendKeyFile,

        /**
         * Authenticate using X.509, accept both keyfiles and X.509
         */
        kSendX509,

        /**
         * Authenticate using X.509, accept only X.509
         */
        kX509
    };

public:
    /**
     * Parse a string and return either a ClusterAuthMode or a not-okay Status.
     */
    static StatusWith<ClusterAuthMode> parse(std::string_view strMode);

    /**
     * Return a pre-constructed ClusterAuthMode for keyFile mode.
     *
     * This is used for implied-key file startup.
     */
    static ClusterAuthMode keyFile() {
        return ClusterAuthMode(Value::kKeyFile);
    }

    /**
     * Get the ClusterAuthMode associated with the given ServiceContext.
     *
     * This function is synchronized.
     */
    static ClusterAuthMode get(ServiceContext* svcCtx);

    /**
     * Set the ClusterAuthMode associated with a ServiceContext and return the previous mode.
     *
     * This function is synchronized.
     */
    static ClusterAuthMode set(ServiceContext* svcCtx, const ClusterAuthMode& mode);

    ClusterAuthMode() = default;
    ~ClusterAuthMode() = default;

    /**
     * Returns true if this mode is allowed to transition to the other mode.
     */
    bool canTransitionTo(const ClusterAuthMode& mode) const;

    /**
     * Returns true if this mode is defined.
     */
    bool isDefined() const;

    /**
     * Returns true if this mode allows x509 authentication as a server.
     */
    bool allowsX509() const;

    /**
     * Returns true if this mode allows key file authentication as a server.
     */
    bool allowsKeyFile() const;

    /**
     * Returns true if this mode sends x509 authentication as a client.
     */
    bool sendsX509() const;

    /**
     * Returns true if this mode sends key file authentication as a client.
     */
    bool sendsKeyFile() const;

    /**
     * Returns true if this mode only sends and receives x509 authentication.
     */
    bool x509Only() const;

    /**
     * Returns true if this mode only sends and receives keyfile authentication.
     */
    bool keyFileOnly() const;

    /**
     * Returns a constant string representing this mode.
     */
    std::string_view toString() const;

    /**
     * Returns if two separate ClusterAuthModes are equivalent.
     */
    bool equals(ClusterAuthMode& rhs) const;

private:
    // For now, we can require that static functions are used to make modes.
    ClusterAuthMode(Value value) : _value(value) {}

    Value _value = Value::kUndefined;
};

}  // namespace mongo
