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

#pragma once

#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/synchronized_value.h"

#include <string>

namespace mongo {

constexpr StringData kOID_CommonName = "2.5.4.3"_sd;
constexpr StringData kOID_CountryName = "2.5.4.6"_sd;
constexpr StringData kOID_LocalityName = "2.5.4.7"_sd;
constexpr StringData kOID_StateName = "2.5.4.8"_sd;
constexpr StringData kOID_OName = "2.5.4.10"_sd;
constexpr StringData kOID_OUName = "2.5.4.11"_sd;

/**
 * Represents a structed X509 certificate subject name.
 * For example: C=US,O=MongoDB,OU=KernelTeam,CN=server
 * would be held as a four element vector of Entries.
 * The first entry of which yould be broken down something like:
 * {{"2.5.4.6", 19, "US"}}.
 * Note that _entries is a vector of vectors to accomodate
 * multi-value RDNs.
 */
class SSLX509Name {
public:
    struct Entry {
        Entry(std::string oid, int type, std::string value)
            : oid(std::move(oid)), type(type), value(std::move(value)) {}
        std::string oid;  // e.g. "2.5.4.8" (ST)
        int type;         // e.g. 19 (PRINTABLESTRING)
        std::string value;
        auto equalityLens() const {
            return std::tie(oid, type, value);
        }
        bool operator==(const Entry& rhs) const {
            return this->equalityLens() == rhs.equalityLens();
        }
    };

    SSLX509Name() = default;
    explicit SSLX509Name(std::vector<std::vector<Entry>> entries) : _entries(std::move(entries)) {}

    /**
     * Retrieve the first instance of the value for a given OID in this name.
     * Returns ErrorCodes::KeyNotFound if the OID does not exist.
     */
    StatusWith<std::string> getOID(StringData oid) const;

    bool empty() const {
        return std::all_of(
            _entries.cbegin(), _entries.cend(), [](const auto& e) { return e.empty(); });
    }

    friend StringBuilder& operator<<(StringBuilder&, const SSLX509Name&);
    std::string toString() const;

    friend bool operator==(const SSLX509Name& lhs, const SSLX509Name& rhs) {
        return lhs._entries == rhs._entries;
    }
    friend bool operator!=(const SSLX509Name& lhs, const SSLX509Name& rhs) {
        return !(lhs._entries == rhs._entries);
    }

    const std::vector<std::vector<Entry>>& entries() const {
        return _entries;
    }

    /*
     * This will go through every entry, verify that it's type is a valid DirectoryString
     * according to https://tools.ietf.org/html/rfc5280#section-4.1.2.4, and perform
     * the RFC 4518 string prep algorithm on it to normalize the values so they can be
     * directly compared. After this, all entries should have the type 12 (utf8String).
     */
    Status normalizeStrings();

    /**
     * A SSLX509Name is said to contain another SSLX509Name if it contains all of the other
     * SSLX509Name's entries.
     */
    bool contains(const SSLX509Name& other) const;

private:
    std::vector<std::vector<Entry>> _entries;
};

std::ostream& operator<<(std::ostream&, const SSLX509Name&);
inline bool operator<(const SSLX509Name::Entry& lhs, const SSLX509Name::Entry& rhs) {
    return lhs.equalityLens() < rhs.equalityLens();
}

class SSLConfiguration {
public:
    bool isClusterMember(StringData subjectName,
                         const boost::optional<std::string>& clusterExtensionValue) const;
    bool isClusterMember(SSLX509Name subjectName,
                         const boost::optional<std::string>& clusterExtensionValue) const;
    void getServerStatusBSON(BSONObjBuilder*) const;
    Status setServerSubjectName(SSLX509Name name);

    // Sets the SSLX509Name representations of net.tls.clusterAuthX509.attributes &
    // tlsClusterAuthX509Override.attributes and the string representations of
    // net.tls.clusterAuthX509.extensionValue & tlsClusterAuthX509Override.extensionValue into the
    // SSLConfiguration.
    Status setClusterAuthX509Config();

    // Returns true if the server currently accepts certificates with a certain value for the
    // clusterMembership extension - either through the config option or the override.
    bool isClusterExtensionSet() const {
        bool containsClusterMembershipConfig = false;
        bool containsOverrideClusterMembershipConfig = false;
        auto visitor = OverloadedVisitor{[](const SSLX509Name&) { return false; },
                                         [](const std::string&) {
                                             return true;
                                         }};

        if (_clusterAuthX509Config._configCriteria) {
            containsClusterMembershipConfig =
                visit(visitor, _clusterAuthX509Config._configCriteria.value());
        }

        if (_clusterAuthX509Config._overrideCriteria) {
            containsOverrideClusterMembershipConfig =
                visit(visitor, _clusterAuthX509Config._overrideCriteria.value());
        }

        return containsClusterMembershipConfig || containsOverrideClusterMembershipConfig;
    }

    const SSLX509Name& serverSubjectName() const {
        return _serverSubjectName;
    }

    SSLX509Name clientSubjectName;
    Date_t serverCertificateExpirationDate;

private:
    SSLX509Name _serverSubjectName;

    struct ClusterAuthX509Config {
        ClusterAuthX509Config() = default;

        // Optionally contains either an SSLX509Name representing net.tls.clusterAuthX509.attributes
        // or a string representing net.tls.clusterAuthX509.extensionValue.
        boost::optional<std::variant<SSLX509Name, std::string>> _configCriteria;

        // Optionally contains either an SSLX509Name representing
        // tlsClusterAuthX509Override.attributes or a string representing
        // tlsClusterAuthX509Override.extensionValue.
        boost::optional<std::variant<SSLX509Name, std::string>> _overrideCriteria;
    };

    ClusterAuthX509Config _clusterAuthX509Config;
};

}  // namespace mongo
