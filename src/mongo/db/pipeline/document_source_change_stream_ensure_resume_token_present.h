/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * This stage is used internally for change streams to ensure that the resume token is in the
 * stream.  It is not intended to be created by the user.
 */
class DocumentSourceChangeStreamEnsureResumeTokenPresent final
    : public DocumentSourceChangeStreamCheckResumability {
public:
    static constexpr StringData kStageName =
        change_stream_constants::stage_names::kEnsureResumeTokenPresent;

    const char* getSourceName() const final;

    StageConstraints constraints(PipelineSplitState) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // This stage neither modifies nor renames any field.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    static boost::intrusive_ptr<DocumentSourceChangeStreamEnsureResumeTokenPresent> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamEnsureResumeTokenPresentToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    /**
     * Use the create static method to create a DocumentSourceChangeStreamEnsureResumeTokenPresent.
     */
    DocumentSourceChangeStreamEnsureResumeTokenPresent(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token);
};
}  // namespace mongo
