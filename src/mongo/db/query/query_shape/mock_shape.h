// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_shape/query_shape.h"

namespace mongo::query_shape {

class MockCmdSpecificShapeComponents : public CmdSpecificShapeComponents {

    void HashValue(absl::HashState state) const override {}
    size_t size() const override {
        return 42;
    }
};

class MockShape : public Shape {
public:
    MockShape() : Shape(NamespaceString::createNamespaceString_forTest("test.testns", {}), {}) {}
    const CmdSpecificShapeComponents& specificComponents() const override {
        return _components;
    }

    void appendCmdSpecificShapeComponents(
        BSONObjBuilder&,
        OperationContext*,
        const query_shape::SerializationOptions& opts) const override {}

private:
    MockCmdSpecificShapeComponents _components;
};
}  // namespace mongo::query_shape
