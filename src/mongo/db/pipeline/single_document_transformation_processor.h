/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/transformer_interface.h"

#include <type_traits>

namespace mongo {

// This class is used by the aggregation framework and streams enterprise module
// to perform the document processing needed for stages that do 1:1 document transformation.
class SingleDocumentTransformationProcessor {
public:
    SingleDocumentTransformationProcessor(std::unique_ptr<TransformerInterface> parsedTransform);

    // Processes the given document and returns the transformed document.
    Document process(const Document& input) const;

    const auto& getTransformer() const {
        return *_parsedTransform;
    }
    auto& getTransformer() {
        return *_parsedTransform;
    }

private:
    // Stores transformation logic.
    std::unique_ptr<TransformerInterface> _parsedTransform;
};

}  // namespace mongo
