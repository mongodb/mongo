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

#include <map>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * ServerStatusSections act as customizable extension points into the
 * output of the serverStatus command. The BSONObj returned by generateSection
 * is appended to the serverStatus response under the name of the section.
 *
 * Use ServerStatusSectionBuilder below to build your ServerStatusSection and register it with the
 * command. Make sure to perform the registration before the server can run commands.
 */
class ServerStatusSection {
public:
    ServerStatusSection(std::string sectionName, ClusterRole role)
        : _sectionName(std::move(sectionName)), _role(role) {}
    virtual ~ServerStatusSection() = default;

    const std::string& getSectionName() const {
        return _sectionName;
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
    using SectionMap = std::map<std::string, std::unique_ptr<ServerStatusSection>>;

    /** Get the singleton ServerStatusSectionRegistry for the process. */
    static ServerStatusSectionRegistry* instance();

    /**
     * Add a status section to the map. Called by ServerStatusSection constructor
     */
    void addSection(std::unique_ptr<ServerStatusSection> section);

    SectionMap::const_iterator begin();

    SectionMap::const_iterator end();

private:
    AtomicWord<bool> _runCalled{false};

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
class ServerStatusSectionBuilder {
public:
    explicit ServerStatusSectionBuilder(std::string name) : _name(std::move(name)) {}

    /** Executes the builder; returns a reference to the built + registered section. */
    Section& operator*() && {
        // Every section must be for eitehr the shard service, router service, or both:
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


class OpCounterServerStatusSection : public ServerStatusSection {
public:
    OpCounterServerStatusSection(const std::string& sectionName,
                                 ClusterRole role,
                                 OpCounters* counters);

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override;

private:
    const OpCounters* _counters;
};

}  // namespace mongo
