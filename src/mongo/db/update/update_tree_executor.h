/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/update/update_executor.h"

#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_object_node.h"
#include "mongo/db/update/v2_log_builder.h"

namespace mongo {

class UpdateTreeExecutor : public UpdateExecutor {
public:
    explicit UpdateTreeExecutor(std::unique_ptr<UpdateObjectNode> node)
        : _updateTree(std::move(node)) {}

    ApplyResult applyUpdate(ApplyParams applyParams) const final {
        mutablebson::Document logDocument;
        boost::optional<v2_log_builder::V2LogBuilder> optV2LogBuilder;

        UpdateNode::UpdateNodeApplyParams updateNodeApplyParams;

        if (applyParams.logMode == ApplyParams::LogMode::kGenerateOplogEntry) {
            optV2LogBuilder.emplace();
            updateNodeApplyParams.logBuilder = optV2LogBuilder.get_ptr();
        }

        auto ret = _updateTree->apply(applyParams, updateNodeApplyParams);

        invariant(ret.oplogEntry.isEmpty());
        if (auto logBuilder = updateNodeApplyParams.logBuilder) {
            ret.oplogEntry = logBuilder->serialize();
        }

        return ret;
    }

    UpdateNode* getUpdateTree() {
        return static_cast<UpdateNode*>(_updateTree.get());
    }

    /**
     * Gather all update operators in the subtree rooted from '_updateTree' into a BSONObj in the
     * format of the update command's update parameter.
     */
    Value serialize() const final {
        return Value(_updateTree->serialize());
    }

    void setCollator(const CollatorInterface* collator) final {
        _updateTree->setCollator(collator);
    }

private:
    std::unique_ptr<UpdateObjectNode> _updateTree;
};

}  // namespace mongo
