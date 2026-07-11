// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_object_node.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/v2_log_builder.h"
#include "mongo/util/modules.h"

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
            if (auto diff = ret.oplogEntry[update_oplog_entry::kDiffObjectFieldName];
                diff.isABSONObj()) {
                ret.diff = diff.embeddedObject();
            }
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
        return serialize(query_shape::SerializationOptions());
    }
    Value serialize(const query_shape::SerializationOptions& opts) const {
        return Value(_updateTree->serialize(opts));
    }

    void setCollator(const CollatorInterface* collator) final {
        _updateTree->setCollator(collator);
    }

private:
    std::unique_ptr<UpdateObjectNode> _updateTree;
};

}  // namespace mongo
