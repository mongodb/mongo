/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/extension/host_connector/adapter/view_info_adapter.h"

#include "mongo/db/extension/shared/byte_buf_utils.h"

namespace mongo::extension::host_connector {

ViewInfoAdapter::ViewInfoAdapter(std::string&& dbName,
                                 std::string&& viewName,
                                 std::vector<mongo::BSONObj>&& viewPipeline,
                                 std::vector<MongoExtensionByteView>&& viewPipelineByteViews)
    : _dbName(std::move(dbName)),
      _viewName(std::move(viewName)),
      _viewPipeline(std::move(viewPipeline)),
      _viewPipelineByteViews(std::move(viewPipelineByteViews)),
      _api({.viewNamespace = {stringViewAsByteView(_dbName), stringViewAsByteView(_viewName)},
            .viewPipelineLen = _viewPipeline.size(),
            .viewPipeline =
                _viewPipelineByteViews.empty() ? nullptr : _viewPipelineByteViews.data()}) {}

ViewInfoAdapter ViewInfoAdapter::fromViewInfo(const ViewInfo& viewInfo) {
    std::string dbStr = DatabaseNameUtil::serialize(viewInfo.getViewName().dbName(),
                                                    SerializationContext::stateCommandRequest());

    std::vector<BSONObj> stages = viewInfo.getOriginalBson();
    std::vector<::MongoExtensionByteView> stageViews;
    stageViews.reserve(stages.size());
    for (const auto& obj : stages) {
        stageViews.push_back(objAsByteView(obj));
    }

    return ViewInfoAdapter(std::move(dbStr),
                           std::string{viewInfo.getViewName().coll()},
                           std::move(stages),
                           std::move(stageViews));
}
}  // namespace mongo::extension::host_connector
