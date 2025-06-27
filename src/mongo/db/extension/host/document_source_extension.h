/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/host/aggregation_stage.h"
#include "mongo/db/pipeline/document_source.h"

namespace mongo {

class DocumentSourceExtensionTest;

namespace extension::host {

/**
 * A DocumentSource implementation for an extension aggregation stage. DocumentSourceExtension is a
 * facade around handles to extension API objects.
 */
class DocumentSourceExtension : public DocumentSource, public exec::agg::Stage {
public:
    const char* getSourceName() const override;

    Id getId() const override;

    GetNextResult doGetNext() override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override;

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    Value serialize(const SerializationOptions& opts) const override;

    // This method is invoked by extensions to register descriptor.
    static void registerStage(ExtensionAggregationStageDescriptorHandle descriptor);

private:
    static void registerStage(
        const std::string& name,
        DocumentSource::Id id,
        extension::host::ExtensionAggregationStageDescriptorHandle descriptor);

    /**
     * Give access to DocumentSourceExtensionTest to unregister parser.
     * unregisterParser_forTest is only meant to be used in the context of unit
     * tests. This is because the parserMap is not thread safe, so modifying it at runtime is
     * unsafe.
     */
    friend class mongo::DocumentSourceExtensionTest;
    static void unregisterParser_forTest(const std::string& name);

    DocumentSourceExtension(
        StringData name,
        boost::intrusive_ptr<ExpressionContext> exprCtx,
        Id id,
        BSONObj rawStage,
        mongo::extension::host::ExtensionAggregationStageDescriptorHandle descriptor);

    // Do not support copy or move.
    DocumentSourceExtension(const DocumentSourceExtension&) = delete;
    DocumentSourceExtension(DocumentSourceExtension&&) = delete;
    DocumentSourceExtension& operator=(const DocumentSourceExtension&) = delete;
    DocumentSourceExtension& operator=(DocumentSourceExtension&&) = delete;

    /**
     * NB : Here we keep a copy of the stage name to service getSourceName().
     * It is tempting to rely on the name which is provided by the _staticDescriptor, however, that
     * is risky because we need to return a const char* in getSourceName() but a
     * MongoExtensionByteView coming from the extension is not guaranteed to contain a null
     * terminator.
     **/
    const std::string _stageName;
    const Id _id;
    BSONObj _raw_stage;
    const mongo::extension::host::ExtensionAggregationStageDescriptorHandle _staticDescriptor;
    mongo::extension::host::ExtensionLogicalAggregationStageHandle _logicalStage;
};
}  // namespace extension::host
}  // namespace mongo
