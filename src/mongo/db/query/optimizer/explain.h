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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/explain_interface.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"

namespace mongo::optimizer {

namespace cascades {
class Memo;
}

/**
 * This structure holds any data that is required by the BSON version of explain. It is
 * self-sufficient and separate because it must outlive the other optimizer state as it is used by
 * the runtime plan executor.
 */
class ABTPrinter : public AbstractABTPrinter {
public:
    ABTPrinter(ABT abtTree, NodeToGroupPropsMap nodeToPropsMap)
        : _abtTree(std::move(abtTree)), _nodeToPropsMap(std::move(nodeToPropsMap)) {}

    BSONObj explainBSON() const override final;

private:
    ABT _abtTree;
    NodeToGroupPropsMap _nodeToPropsMap;
};

class ExplainGenerator {
public:
    // Optionally display logical and physical properties using the memo.
    // whenever memo delegators are printed.
    static std::string explain(const ABT& node,
                               bool displayProperties = false,
                               const cascades::Memo* memo = nullptr,
                               const NodeToGroupPropsMap& nodeMap = {});

    // Optionally display logical and physical properties using the memo.
    // whenever memo delegators are printed.
    static std::string explainV2(const ABT& node,
                                 bool displayProperties = false,
                                 const cascades::Memo* memo = nullptr,
                                 const NodeToGroupPropsMap& nodeMap = {});

    // Optionally display logical and physical properties using the memo.
    // whenever memo delegators are printed.
    static std::string explainV2Compact(const ABT& node,
                                        bool displayProperties = false,
                                        const cascades::Memo* memo = nullptr,
                                        const NodeToGroupPropsMap& nodeMap = {});

    static std::string explainNode(const ABT& node);

    static std::pair<sbe::value::TypeTags, sbe::value::Value> explainBSON(
        const ABT& node,
        bool displayProperties = false,
        const cascades::Memo* memo = nullptr,
        const NodeToGroupPropsMap& nodeMap = {});

    static BSONObj explainBSONObj(const ABT& node,
                                  bool displayProperties = false,
                                  const cascades::Memo* memo = nullptr,
                                  const NodeToGroupPropsMap& nodeMap = {});

    static std::string printBSON(sbe::value::TypeTags tag, sbe::value::Value val);

    static std::string explainLogicalProps(const std::string& description,
                                           const properties::LogicalProps& props);
    static std::string explainPhysProps(const std::string& description,
                                        const properties::PhysProps& props);

    static std::string explainMemo(const cascades::Memo& memo);

    static std::pair<sbe::value::TypeTags, sbe::value::Value> explainMemoBSON(
        const cascades::Memo& memo);

    static std::string explainPartialSchemaReqMap(const PartialSchemaRequirements& reqMap);

    static std::string explainInterval(const IntervalRequirement& interval);

    static std::string explainIntervalExpr(const IntervalReqExpr::Node& intervalExpr);
};

}  // namespace mongo::optimizer
