/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"

namespace mongo {

class DocumentSourceSetWindowFields final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$setWindowFields"_sd;

    /**
     * Parses 'elem' into a $setWindowFields stage, or throws a AssertionException if 'elem' was an
     * invalid specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);


    DocumentSourceSetWindowFields(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  boost::optional<boost::intrusive_ptr<Expression>> partitionBy,
                                  boost::optional<BSONObj> sortBy,
                                  BSONObj fields)
        : DocumentSource(kStageName, expCtx),
          _partitionBy(partitionBy),
          _sortBy(std::move(sortBy)),
          _fields(std::move(fields)) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return StageConstraints(StreamType::kBlocking,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kWritesTmpData,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed);
    }

    const char* getSourceName() const {
        return kStageName.rawData();
    };

    boost::optional<DistributedPlanLogic> distributedPlanLogic() {
        // Force to run on the merging half for now.
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const;

    DocumentSource::GetNextResult doGetNext();

private:
    boost::optional<boost::intrusive_ptr<Expression>> _partitionBy;
    boost::optional<BSONObj> _sortBy;
    BSONObj _fields;
};

}  // namespace mongo
