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

#include "mongo/db/pipeline/visitors/transformer_interface_walker.h"
#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_replace_root.h"

namespace mongo {

TransformerInterfaceWalker::TransformerInterfaceWalker(TransformerInterfaceConstVisitor* visitor)
    : _visitor(visitor) {}

void TransformerInterfaceWalker::walk(const TransformerInterface* transformer) {
    switch (transformer->getType()) {
        case TransformerInterface::TransformerType::kExclusionProjection:
            _visitor->visit(
                static_cast<const projection_executor::ExclusionProjectionExecutor*>(transformer));
            break;

        case TransformerInterface::TransformerType::kInclusionProjection:
            _visitor->visit(
                static_cast<const projection_executor::InclusionProjectionExecutor*>(transformer));
            break;

        case TransformerInterface::TransformerType::kComputedProjection:
            _visitor->visit(
                static_cast<const projection_executor::AddFieldsProjectionExecutor*>(transformer));
            break;

        case TransformerInterface::TransformerType::kReplaceRoot:
            _visitor->visit(static_cast<const ReplaceRootTransformation*>(transformer));
            break;

        case TransformerInterface::TransformerType::kGroupFromFirstDocument:
            _visitor->visit(static_cast<const GroupFromFirstDocumentTransformation*>(transformer));
            break;

        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace mongo
