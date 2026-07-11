// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_shape/mock_shape.h"
#include "mongo/db/query/query_stats/key.h"

namespace mongo::query_stats {
class MockKey : public Key {
public:
    MockKey(OperationContext* opCtx)
        : Key(opCtx, std::make_unique<query_shape::MockShape>(), boost::none, boost::none, false) {}

    const SpecificKeyComponents& specificComponents() const override {
        return _components;
    }

    void appendCommandSpecificComponents(
        BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const override {}

private:
    EmptyCmdComponents _components;
};
}  // namespace mongo::query_stats
