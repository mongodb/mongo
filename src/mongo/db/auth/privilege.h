// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class Privilege;
class TenantId;

using PrivilegeVector = std::vector<Privilege>;

namespace auth {
class ParsedPrivilege;
}  // namespace auth

/**
 * A representation of the permission to perform a set of actions on a resource.
 */
class Privilege {
public:
    /**
     * Adds "privilegeToAdd" to "privileges", de-duping "privilegeToAdd" if the vector already
     * contains a privilege on the same resource.
     *
     * This method is the preferred way to add privileges to  privilege vectors.
     */
    static void addPrivilegeToPrivilegeVector(PrivilegeVector* privileges,
                                              const Privilege& privilegeToAdd);

    static void addPrivilegesToPrivilegeVector(PrivilegeVector* privileges,
                                               const PrivilegeVector& privilegesToAdd);

    /**
     * Promote a vector of ParsedPrivilege documents into tenant aware privileges.
     */
    static PrivilegeVector privilegeVectorFromParsedPrivilegeVector(
        const boost::optional<TenantId>&,
        const std::vector<auth::ParsedPrivilege>&,
        std::vector<std::string>*);

    /**
     * Takes a vector of privileges and fills the output param "resultArray" with a BSON array
     * representation of the privileges.
     */
    static void serializePrivilegeVector(const PrivilegeVector& privileges, BSONArrayBuilder*);


    Privilege() = default;
    ~Privilege() = default;

    Privilege(const Privilege&) = default;
    Privilege& operator=(const Privilege&) = default;

    Privilege(Privilege&&) = default;
    Privilege& operator=(Privilege&&) = default;

    Privilege(const ResourcePattern& resource, ActionType action);
    Privilege(const ResourcePattern& resource, ActionSet actions);

    // Transform a ParsedPrivilege into a concrete Privilege by adding tenantId
    // and turning string actions into ActionSet bits.
    // unrecognizedActions will be populated with unexpected ActionType names, if present.
    static Privilege resolvePrivilegeWithTenant(
        const boost::optional<TenantId>&,
        const auth::ParsedPrivilege&,
        std::vector<std::string>* unrecognizedActions = nullptr);

    const ResourcePattern& getResourcePattern() const {
        return _resource;
    }

    const ActionSet& getActions() const {
        return _actions;
    }

    auth::ParsedPrivilege toParsedPrivilege() const;

    void addActions(const ActionSet& actionsToAdd);
    void removeActions(const ActionSet& actionsToRemove);

    // Checks if the given action is present in the Privilege.
    bool includesAction(ActionType action) const;
    // Checks if the given actions are present in the Privilege.
    bool includesActions(const ActionSet& actions) const;

    void serialize(BSONObjBuilder*) const;
    BSONObj toBSON() const;

private:
    ResourcePattern _resource;
    ActionSet _actions;  // bitmask of actions this privilege grants
};

inline bool operator==(const Privilege& lhs, const Privilege& rhs) {
    return lhs.getResourcePattern() == rhs.getResourcePattern() &&
        lhs.getActions() == rhs.getActions();
}

}  // namespace mongo
