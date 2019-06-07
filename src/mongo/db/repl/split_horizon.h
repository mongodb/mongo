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

#include <map>
#include <string>

#include <boost/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace repl {

/**
 * Every Replica Set member has several views under which it can respond.  The Split Horizon class
 * represents the unification of all of those views.  For example, a member might be reachable under
 * "internal.example.com:27017" and "external.example.com:25000".  The replica set needs to be able
 * to respond, as a group, with the correct view, when `isMaster` requests come in.  Each member of
 * the replica set has its own `SplitHorizon` class to manage the mapping between server names and
 * horizon names.  `SplitHorizon` models a single member's view across all horizons, not views for
 * all of the members.
 */
class SplitHorizon {
public:
    static constexpr auto kDefaultHorizon = "__default"_sd;

    using ForwardMapping = StringMap<HostAndPort>;
    using ReverseHostOnlyMapping = std::map<std::string, std::string>;

    using AllMappings =
        std::tuple<SplitHorizon::ForwardMapping, SplitHorizon::ReverseHostOnlyMapping>;

    struct Parameters {
        boost::optional<std::string> sniName;

        Parameters() = default;
        explicit Parameters(boost::optional<std::string> initialSniName)
            : sniName(std::move(initialSniName)) {}
    };

    /**
     * Set the split horizon connection parameters, for use by future `isMaster` commands.
     */
    static void setParameters(Client* client, boost::optional<std::string> sniName);

    /**
     * Get the client's SplitHorizonParameters object.
     */
    static Parameters getParameters(const Client*);

    explicit SplitHorizon() = default;
    explicit SplitHorizon(const HostAndPort& host,
                          const boost::optional<BSONElement>& horizonsElement);

    // This constructor is for testing and internal use only
    explicit SplitHorizon(ForwardMapping forward);

    /**
     * Gets the horizon name for which the parameters (captured during the first `isMaster`)
     * correspond.
     */
    StringData determineHorizon(const Parameters& horizonParameters) const;

    const HostAndPort& getHostAndPort(StringData horizon) const {
        invariant(!_forwardMapping.empty());
        invariant(!horizon.empty());
        auto found = _forwardMapping.find(horizon);
        if (found == end(_forwardMapping))
            uasserted(ErrorCodes::NoSuchKey, str::stream() << "No horizon named " << horizon);
        return found->second;
    }

    const auto& getForwardMappings() const {
        return _forwardMapping;
    }

    const auto& getReverseHostMappings() const {
        return _reverseHostMapping;
    }

    void toBSON(BSONObjBuilder& configBuilder) const;

private:
    // Unified Constructor -- All other constructors delegate to this one.
    explicit SplitHorizon(AllMappings mappings)
        : _forwardMapping(std::move(std::get<0>(mappings))),
          _reverseHostMapping(std::move(std::get<1>(mappings))) {}

    // Maps each horizon name to a network address for this replica set member
    ForwardMapping _forwardMapping;

    // Maps each hostname which this replica set member has to a horizon name under which that
    // address applies
    ReverseHostOnlyMapping _reverseHostMapping;
};
}  // namespace repl
}  // namespace mongo
