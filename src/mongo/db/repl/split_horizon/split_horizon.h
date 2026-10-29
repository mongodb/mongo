// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {
using namespace std::literals::string_view_literals;

/**
 * Every Replica Set member has several views under which it can respond.  The Split Horizon class
 * represents the unification of all of those views.  For example, a member might be reachable under
 * "internal.example.com:27017" and "external.example.com:25000".  The replica set needs to be able
 * to respond, as a group, with the correct view, when hello requests come in.  Each member of
 * the replica set has its own `SplitHorizon` class to manage the mapping between server names and
 * horizon names.  `SplitHorizon` models a single member's view across all horizons, not views for
 * all of the members.
 */
class SplitHorizon {
public:
    static constexpr auto kDefaultHorizon = "__default"sv;

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
     * Set the split horizon connection parameters, for use by future `hello/isMaster` commands.
     */
    static void setParameters(Client* client, boost::optional<std::string> sniName);

    /**
     * Get the client's SplitHorizonParameters object.
     */
    static Parameters getParameters(const Client*);

    explicit SplitHorizon() = default;
    explicit SplitHorizon(const HostAndPort& host, const boost::optional<BSONObj>& horizonsObject);

    // This constructor is for testing and internal use only
    explicit SplitHorizon(ForwardMapping forward);

    /**
     * Gets the horizon name for which the parameters (captured during the first `isMaster`)
     * correspond.
     */
    std::string determineHorizon(const Parameters& horizonParameters) const;

    const HostAndPort& getHostAndPort(std::string_view horizon) const {
        invariant(!_forwardMapping.empty());
        invariant(!horizon.empty());
        auto found = _forwardMapping.find(horizon);
        if (found == _forwardMapping.end())
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
