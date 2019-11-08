/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/client/sdam/server_description.h"

namespace mongo::sdam {

/**
 * This class is used in the unit tests to construct ServerDescription instances. For production
 * code, ServerDescription instances should be constructed using its constructors.
 */
class ServerDescriptionBuilder {
public:
    ServerDescriptionBuilder() = default;

    /**
     * Return the configured ServerDescription instance.
     */
    ServerDescriptionPtr instance() const;

    // server identity
    ServerDescriptionBuilder& withAddress(const ServerAddress& address);
    ServerDescriptionBuilder& withType(const ServerType type);
    ServerDescriptionBuilder& withMe(const ServerAddress& me);
    ServerDescriptionBuilder& withTag(const std::string key, const std::string value);
    ServerDescriptionBuilder& withSetName(const std::string setName);

    // network attributes
    ServerDescriptionBuilder& withRtt(const IsMasterRTT& rtt);
    ServerDescriptionBuilder& withError(const std::string& error);
    ServerDescriptionBuilder& withLogicalSessionTimeoutMinutes(
        const boost::optional<int> logicalSessionTimeoutMinutes);

    // server capabilities
    ServerDescriptionBuilder& withMinWireVersion(int minVersion);
    ServerDescriptionBuilder& withMaxWireVersion(int maxVersion);

    // server 'time'
    ServerDescriptionBuilder& withLastWriteDate(const Date_t& lastWriteDate);
    ServerDescriptionBuilder& withOpTime(const repl::OpTime opTime);
    ServerDescriptionBuilder& withLastUpdateTime(const Date_t& lastUpdateTime);

    // topology membership
    ServerDescriptionBuilder& withPrimary(const ServerAddress& primary);
    ServerDescriptionBuilder& withHost(const ServerAddress& host);
    ServerDescriptionBuilder& withPassive(const ServerAddress& passive);
    ServerDescriptionBuilder& withArbiter(const ServerAddress& arbiter);
    ServerDescriptionBuilder& withSetVersion(const int setVersion);
    ServerDescriptionBuilder& withElectionId(const OID& electionId);

private:
    constexpr static auto kServerAddressNotSet = "address.not.set:1234";
    ServerDescriptionPtr _instance =
        std::shared_ptr<ServerDescription>(new ServerDescription(kServerAddressNotSet));
};
}  // namespace mongo::sdam
