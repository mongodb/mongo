// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/util/modules.h"

namespace mongo::query_shape {

struct CountCmdShapeComponents : public CmdSpecificShapeComponents {

    // The 'hasLimit' and 'hasSkip' parameters are required to initialize the hasField member
    // variable because 'request' never includes the skip or limit, even if the count command
    // contains a skip and/or limit. See the comment in parsed_find_command::parseFromCount for
    // more information.
    CountCmdShapeComponents(const ParsedFindCommand& request, bool hasLimit, bool hasSkip);

    void HashValue(absl::HashState state) const final;

    size_t size() const final;

    // This anonymous struct represents the presence of the member variables as C++ bit fields.
    // In doing so, each of these boolean values takes up 1 bit instead of 1 byte.
    const struct HasField {
        bool limit : 1;
        bool skip : 1;
    } hasField;

    const BSONObj representativeQuery;
};

class CountCmdShape final : public Shape {
public:
    CountCmdShape(const ParsedFindCommand& find,
                  bool hasLimit,
                  bool hasSkip,
                  bool rawData = false);  // rawData is stored in Shape base class

    const CmdSpecificShapeComponents& specificComponents() const final;

    void appendCmdSpecificShapeComponents(
        BSONObjBuilder&,
        OperationContext*,
        const query_shape::SerializationOptions& opts) const final;

    QueryShapeHash sha256Hash(OperationContext*,
                              const SerializationContext& serializationContext) const override;

    const CountCmdShapeComponents components;
};

static_assert(sizeof(CountCmdShape) == sizeof(Shape) + sizeof(CountCmdShapeComponents),
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");
}  // namespace mongo::query_shape
