// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
namespace repl {

class [[MONGO_MOD_PARENT_PRIVATE]] ReplSetWriteConcernModeDefinitions {
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

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const;
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
