// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <map>
#include <string>

namespace mongo {

/**
 * ServerStatusSections act as customizable extension points into the
 * output of the serverStatus command. The BSONObj returned by generateSection
 * is appended to the serverStatus response under the name of the section.
 *
 * Use ServerStatusSectionBuilder below to build your ServerStatusSection and register it with the
 * command. Make sure to perform the registration before the server can run commands.
 */
class [[MONGO_MOD_OPEN]] ServerStatusSection {
public:
    ServerStatusSection(std::string sectionName, ClusterRole role)
        : _sectionName(std::move(sectionName)), _role(role) {}
    virtual ~ServerStatusSection() = default;

    const std::string& getSectionName() const {
        return _sectionName;
    }

    ClusterRole getClusterRole() const {
        return _role;
    }

    /**
     * True if the section is relevant to `serviceRole`. `serviceRole` must be one of _exactly_
     * ClusterRole::ShardServer or ClusterRole::RouterServer, and it holds the ClusterRole that some
     * Service is associated with. A ServerStatusSection is relevant to a service if the section's
     * _role has any overlap with the `serviceRole` for the service.
     */
    bool relevantTo(const ClusterRole& serviceRole) const {
        constexpr static std::array allRoles{ClusterRole::ShardServer, ClusterRole::RouterServer};
        // TODO SERVER-87512 - Once all roles are tagged, remove the exception for
        // ClusterRole::None. Sections not tagged with a role are relevant to all services.
        return std::any_of(allRoles.begin(),
                           allRoles.end(),
                           [&](auto&& r) { return _role.has(r) && serviceRole.has(r); }) ||
            _role.hasExclusively(ClusterRole::None);
    }

    /**
     * if this returns true, if the user doesn't mention this section
     * it will be included in the result
     * if they do : 1, it will be included
     * if they do : 0, it will not
     *
     * examples (section 'foo')
     *  includeByDefault returning true
     *     foo : 0 = not included
     *     foo : 1 = included
     *     foo missing = included
     *  includeByDefault returning false
     *     foo : 0 = not included
     *     foo : 1 = included
     *     foo missing = false
     */
    virtual bool includeByDefault() const = 0;

    /**
     * Perform authorization checks required to show this status section.
     * TODO: Remove this empty default implementation and implement for every section.
     */
    virtual Status checkAuthForOperation(OperationContext* opCtx) const {
        return Status::OK();
    }

    /**
     * actually generate the result
     *
     * You should either implement this function or appendSection below, but not both. In
     * most cases you should just implement this function.
     *
     * @param configElement the element from the actual command related to this section
     *                      so if the section is 'foo', this is cmdObj['foo']
     */
    virtual BSONObj generateSection(OperationContext* opCtx,
                                    const BSONElement& configElement) const {
        return BSONObj{};
    };

    /**
     * This is what gets called by the serverStatus command to append the section to the
     * command result.
     *
     * If you are just implementing a normal ServerStatusSection, then you don't need to
     * implement this.
     *
     * If you are doing something a bit more complicated, you can implement this and have
     * full control over what gets included in the command result.
     */
    virtual void appendSection(OperationContext* opCtx,
                               const BSONElement& configElement,
                               BSONObjBuilder* result) const {
        const auto ret = generateSection(opCtx, configElement);
        if (ret.isEmpty())
            return;
        result->append(getSectionName(), ret);
    }

private:
    const std::string _sectionName;
    const ClusterRole _role;
};

/**
 * Class the keeps a map of all registered status sections
 */
class ServerStatusSectionRegistry {
public:
    enum class RoleTag { shard, router, shardAndRouter };
    using KeyType = std::pair<std::string, RoleTag>;
    using SectionMap = std::map<KeyType, std::unique_ptr<ServerStatusSection>>;

    /** Get the singleton ServerStatusSectionRegistry for the process. */
    static ServerStatusSectionRegistry* instance();

    /**
     * Add a status section to the map. Called by ServerStatusSection constructor
     */
    void addSection(std::unique_ptr<ServerStatusSection> section);

    SectionMap::const_iterator begin();

    SectionMap::const_iterator end();

private:
    RoleTag getTagForRole(ClusterRole);

    Atomic<bool> _runCalled{false};
    SectionMap _sections;
};

/**
 * Builder that can be used to construct ServerStatusSections and register them with the global
 * registry used by the serverStatus command.
 *
 * Builds an instance of the section which is owned and stored by the registry, and returns
 * a reference to the built section. To use create a variable at namespace scope to store
 * the reference:
 *
 * auto& myInstance = *ServerStatusSectionBuilder<MySectionType>("mySectionName");
 *
 * ServerStatusSections can be associated with a cluster-role in the registry, so that
 * mongod running with an embedded-router mode can correctly track shard vs. router metrics.
 *
 * If your section takes custom constructor args, you can use `bind` to forward them.
 * Your section must then override the base-class constructor, and pass it the sectionName
 * and ClusterRole.
 *
 * operator* must be invoked/the build must be exected before the server can run commands, to ensure
 * all sections are registered before serverStatus can be run.
 */
template <typename Section>
class [[MONGO_MOD_PUBLIC]] ServerStatusSectionBuilder {
public:
    explicit ServerStatusSectionBuilder(std::string name) : _name(std::move(name)) {}

    /** Executes the builder; returns a reference to the built + registered section. */
    Section& operator*() && {
        // Every section must be for either the shard service, router service, or both:
        // invariant(_roles.has(ClusterRole::RouterServer) || _roles.has(ClusterRole::ShardServer));
        std::unique_ptr<Section> section;
        if (_construct) {
            section = _construct();
        } else {
            if constexpr (std::is_constructible_v<Section, std::string, ClusterRole>) {
                section = std::make_unique<Section>(_name, _roles);
            } else {
                invariant(false, "No suitable constructor");
            }
        }
        auto& ref = *section;
        ServerStatusSectionRegistry::instance()->addSection(std::move(section));
        return ref;
    }

    /** Used to associate the section with the router-role. */
    ServerStatusSectionBuilder forRouter() && {
        _roles += ClusterRole::RouterServer;
        return std::move(*this);
    }

    /** Used to associate the section with the shard-role. */
    ServerStatusSectionBuilder forShard() && {
        _roles += ClusterRole::ShardServer;
        return std::move(*this);
    }

    template <typename... Args>
    ServerStatusSectionBuilder bind(Args... args) && {
        _construct = [name = _name, roles = _roles, args...] {
            return std::make_unique<Section>(name, roles, args...);
        };
        return std::move(*this);
    }

private:
    /** Name of the section we are building. */
    std::string _name;
    /** Must be 'shard', 'router', or 'shard+router' when *this runs. */
    ClusterRole _roles{ClusterRole::None};
    /** Used to construct sections that take custom arguments. */
    std::function<std::unique_ptr<Section>()> _construct;
};


class [[MONGO_MOD_PUBLIC]] OpCounterServerStatusSection : public ServerStatusSection {
public:
    OpCounterServerStatusSection(const std::string& sectionName,
                                 ClusterRole role,
                                 OpCounters* counters)
        : ServerStatusSection(sectionName, role), _counters(counters) {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        return _counters->getObj();
    }

private:
    const OpCounters* _counters;
};

/**
 * Publishes an infrequently-changing BSON object as a server status section.
 */
class [[MONGO_MOD_PUBLIC]] BSONObjectStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext*, const BSONElement&) const override {
        return **_obj;
    }

    void setObject(BSONObj obj) {
        **_obj = std::move(obj);
    }

private:
    synchronized_value<BSONObj> _obj;
};

}  // namespace mongo
