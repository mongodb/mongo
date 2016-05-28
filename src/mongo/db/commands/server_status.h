// server_status.h

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
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
    virtual ~ServerStatusSection() {}

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
     * @param configElement the element from the actual command related to this section
     *                      so if the section is 'foo', this is cmdObj['foo']
     */
    virtual BSONObj generateSection(OperationContext* txn,
                                    const BSONElement& configElement) const = 0;

private:
    const std::string _sectionName;
};

class OpCounterServerStatusSection : public ServerStatusSection {
public:
    OpCounterServerStatusSection(const std::string& sectionName, OpCounters* counters);
    virtual bool includeByDefault() const {
        return true;
    }

    virtual BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const;

private:
    const OpCounters* _counters;
};
}
