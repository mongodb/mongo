/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_shape/query_shape.h"

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
    CountCmdShape(const ParsedFindCommand& find, bool hasLimit, bool hasSkip);

    const CmdSpecificShapeComponents& specificComponents() const final;

    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const SerializationOptions& opts) const final;

    QueryShapeHash sha256Hash(OperationContext*,
                              const SerializationContext& serializationContext) const override;

    const CountCmdShapeComponents components;
};

static_assert(sizeof(CountCmdShape) == sizeof(Shape) + sizeof(CountCmdShapeComponents),
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");
}  // namespace mongo::query_shape
