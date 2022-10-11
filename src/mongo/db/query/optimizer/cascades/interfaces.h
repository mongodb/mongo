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

#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/props.h"

namespace mongo::optimizer::cascades {

class Memo;

/**
 * Interface for deriving properties. Typically we supply memo, but may derive without a memo as
 * long as metadata is provided.
 */
class LogicalPropsInterface {
public:
    virtual ~LogicalPropsInterface() = default;

    using NodePropsMap = opt::unordered_map<const Node*, properties::LogicalProps>;

    virtual properties::LogicalProps deriveProps(const Metadata& metadata,
                                                 ABT::reference_type nodeRef,
                                                 NodePropsMap* nodePropsMap = nullptr,
                                                 const Memo* memo = nullptr,
                                                 GroupIdType groupId = -1) const = 0;
};

/**
 * Interface for deriving CE for a newly added logical node in a new memo group.
 */
class CEInterface {
public:
    virtual ~CEInterface() = default;

    virtual CEType deriveCE(const Metadata& metadata,
                            const Memo& memo,
                            const properties::LogicalProps& logicalProps,
                            ABT::reference_type logicalNodeRef) const = 0;
};

/**
 * Interface for deriving costs and adjusted CE (based on physical props) for a physical node.
 */
class CostingInterface {
public:
    virtual ~CostingInterface() = default;
    virtual CostAndCE deriveCost(const Metadata& metadata,
                                 const Memo& memo,
                                 const properties::PhysProps& physProps,
                                 ABT::reference_type physNodeRef,
                                 const ChildPropsType& childProps,
                                 const NodeCEMap& nodeCEMap) const = 0;
};

}  // namespace mongo::optimizer::cascades
