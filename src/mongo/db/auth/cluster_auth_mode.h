/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"

namespace mongo {

class ServiceContext;

/**
 * ClusterAuthMode is a thin wrapper around an enum for decorated storage and semantic utility.
 */
class ClusterAuthMode {
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
    static StatusWith<ClusterAuthMode> parse(StringData strMode);

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
    StringData toString() const;

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
