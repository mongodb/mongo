/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace repl {

class ReplSetWriteConcernModeDefinitions {
public:
    // Default constructor for use as an IDL default.
    ReplSetWriteConcernModeDefinitions() {}

    /**
     * A constraint maps a tag (defined in the "members" section of the ReplSetConfig) to an integer
     * specifying the number of nodes matching that tag which need to accept the write before the
     * constraint is satisfied.
     */
    typedef std::pair<std::string, std::int32_t> Constraint;

    /**
     * A write concern mode is defined by a list of constraints.
     */
    typedef std::vector<Constraint> Definition;

    /**
     * This maps a write concern mode name to the list of constraints which defines it.
     */
    typedef StringMap<Definition> Definitions;

    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;
    static ReplSetWriteConcernModeDefinitions parseFromBSON(BSONElement patternMapElement);

    /**
     * Returns a StringMap of ReplSetTagPatterns, created from these definitions using the passed-in
     * tagConfig.  Will fail if the constraints don't correspond to existing tags in the tagConfig.
     */
    StatusWith<StringMap<ReplSetTagPattern>> convertToTagPatternMap(
        ReplSetTagConfig* tagConfig) const;

private:
    ReplSetWriteConcernModeDefinitions(Definitions&& definitions)
        : _definitions(std::move(definitions)) {}

    Definitions _definitions;
};

}  // namespace repl
}  // namespace mongo
