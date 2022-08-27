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

#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic_word.h"
#include <string>

namespace mongo {

class ServerStatusSection {
public:
    ServerStatusSection(const std::string& sectionName);
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
     * Adds the privileges that are required to view this section
     * TODO: Remove this empty default implementation and implement for every section.
     */
    virtual void addRequiredPrivileges(std::vector<Privilege>* out){};

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
};

/**
 * Class the keeps a map of all registered status sections
 */
class ServerStatusSectionRegistry {
public:
    using SectionMap = std::map<std::string, ServerStatusSection*>;

    static ServerStatusSectionRegistry* get();

    /**
     * Add a status section to the map. Called by ServerStatusSection constructor
     */
    void addSection(ServerStatusSection* section);

    SectionMap::const_iterator begin();

    SectionMap::const_iterator end();

private:
    AtomicWord<bool> _runCalled{false};

    SectionMap _sections;
};

class OpCounterServerStatusSection : public ServerStatusSection {
public:
    OpCounterServerStatusSection(const std::string& sectionName, OpCounters* counters);

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override;

private:
    const OpCounters* _counters;
};

}  // namespace mongo
