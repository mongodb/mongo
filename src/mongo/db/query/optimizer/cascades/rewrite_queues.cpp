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

#include "mongo/db/query/optimizer/cascades/rewrite_queues.h"
#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"
#include <mongo/db/query/optimizer/defs.h>

namespace mongo::optimizer::cascades {

LogicalRewriteEntry::LogicalRewriteEntry(const double priority,
                                         const LogicalRewriteType type,
                                         MemoLogicalNodeId nodeId)
    : _priority(priority), _type(type), _nodeId(nodeId) {}

bool LogicalRewriteEntryComparator::operator()(
    const std::unique_ptr<LogicalRewriteEntry>& x,
    const std::unique_ptr<LogicalRewriteEntry>& y) const {
    // Lower numerical priority is considered last (and thus de-queued first).
    if (x->_priority > y->_priority) {
        return true;
    } else if (x->_priority < y->_priority) {
        return false;
    }

    // Make sure entries in the queue are consistently ordered.
    if (x->_nodeId._groupId < y->_nodeId._groupId) {
        return true;
    } else if (x->_nodeId._groupId > y->_nodeId._groupId) {
        return false;
    }
    return x->_nodeId._index < y->_nodeId._index;
}

void optimizeChildrenNoAssert(PhysRewriteQueue& queue,
                              const double priority,
                              const PhysicalRewriteType rule,
                              ABT node,
                              ChildPropsType childProps,
                              NodeCEMap nodeCEMap) {
    queue.emplace(std::make_unique<PhysRewriteEntry>(
        priority, rule, std::move(node), std::move(childProps), std::move(nodeCEMap)));
}

}  // namespace mongo::optimizer::cascades
