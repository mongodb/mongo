/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

/**
 * Represents a tree of paths with attached properties. For example adding "a.b" and "a.c" results
 * in a root node "a" with two children "b" and "c". By setting appropriate properties we can create
 * a path tree for a $project expression:
 *   Field "a" * Keep "a"
 *      |
 *      Traverse
 *        |
 *        Obj * Keep "b", "c"
 */
struct FieldMapEntry {
    FieldMapEntry(std::string fieldName) : _fieldName(std::move(fieldName)) {
        uassert(6624200, "Empty field name", !_fieldName.empty());
    }

    std::string _fieldName;
    bool _hasKeep = false;
    bool _hasLeadingObj = false;
    bool _hasTrailingDefault = false;
    bool _hasDrop = false;
    std::string _constVarName;

    // TODO SERVER-66846: Consider maintaining children as a vector of FieldMapEntry's. Then we can
    // remove the _fieldMap member of FieldMapBuilder.
    // Child paths are potentially dotted field paths.
    std::set<std::string> _childPaths;
};

class FieldMapBuilder {
    static constexpr const char* kRootElement = "$root";

public:
    FieldMapBuilder(const ProjectionName& rootProjName, bool isRootSameAsScanProj)
        : _rootProjName(rootProjName), _isRootSameAsScanProj(isRootSameAsScanProj) {}

    /**
     * Adds 'fieldPath' as a projected field, creating FieldMapEntries for each element along the
     * path as necessary, and applying 'fn' to each created FieldMapEntry.
     */
    void integrateFieldPath(
        const FieldPath& fieldPath,
        const std::function<void(const bool isLastElement, FieldMapEntry& entry)>& fn);

    /**
     * Produce an ABT representing all fields integrated so far under a single EvalPath. For
     * example, assuming "a.b" and "a.c" were integrated as fields in an inclusion projection, an
     * output may look like:
     *  EvalPath
     *  |                        |
     *  Field "a" * Keep "a"     Variable[rootProjName]
     *      |
     *      Traverse
     *        |
     *        Obj * Keep "b", "c"
     *
     * Returns boost::none when no fields have been integrated so far.
     */
    boost::optional<ABT> generateABT() const;

private:
    ABT generateABTForField(const FieldMapEntry& entry) const;

    const ProjectionName& _rootProjName;
    const bool _isRootSameAsScanProj;

    // Maps from potentially dotted field path to FieldMapEntry.
    opt::unordered_map<std::string, FieldMapEntry> _fieldMap;
};

}  // namespace mongo::optimizer
