// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/views/view.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string_view>
#include <utility>

namespace mongo {

ViewDefinition::ViewDefinition(const DatabaseName& dbName,
                               std::string_view viewName,
                               std::string_view viewOnName,
                               const BSONObj& pipeline,
                               std::unique_ptr<CollatorInterface> collator)
    : _viewNss(NamespaceStringUtil::deserialize(dbName, viewName)),
      _viewOnNss(NamespaceStringUtil::deserialize(dbName, viewOnName)),
      _collator(std::move(collator)) {
    for (BSONElement e : pipeline) {
        _pipeline.push_back(e.Obj().getOwned());
    }
}

ViewDefinition::ViewDefinition(const ViewDefinition& other)
    : _viewNss(other._viewNss),
      _viewOnNss(other._viewOnNss),
      _collator(CollatorInterface::cloneCollator(other._collator.get())),
      _pipeline(other._pipeline) {}

ViewDefinition& ViewDefinition::operator=(const ViewDefinition& other) {
    if (this == &other)
        return *this;
    _viewNss = other._viewNss;
    _viewOnNss = other._viewOnNss;
    _collator = CollatorInterface::cloneCollator(other._collator.get());
    _pipeline = other._pipeline;

    return *this;
}

void ViewDefinition::setViewOn(const NamespaceString& viewOnNss) {
    invariant(_viewNss.isEqualDb(viewOnNss));
    _viewOnNss = viewOnNss;
}

void ViewDefinition::setPipeline(std::vector<mongo::BSONObj> pipeline) {
    for (auto& stage : pipeline) {
        stage = stage.copy();
    }
    _pipeline.swap(pipeline);
}
}  // namespace mongo
