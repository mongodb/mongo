/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <deque>

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

/**
 * Used in testing to store documents without using the storage layer. Methods are not marked as
 * final in order to allow tests to intercept calls if needed.
 */
class DocumentSourceMock : public DocumentSource {
public:
    DocumentSourceMock(std::deque<GetNextResult> results);
    DocumentSourceMock(std::deque<GetNextResult> results,
                       const boost::intrusive_ptr<ExpressionContext>& expCtx);

    GetNextResult getNext() override;
    const char* getSourceName() const override;
    Value serialize(
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const override;

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed);

        constraints.requiresInputDocSource = false;
        return constraints;
    }

    BSONObjSet getOutputSorts() override {
        return sorts;
    }

    static boost::intrusive_ptr<DocumentSourceMock> create();

    static boost::intrusive_ptr<DocumentSourceMock> create(Document doc);

    static boost::intrusive_ptr<DocumentSourceMock> create(const GetNextResult& result);
    static boost::intrusive_ptr<DocumentSourceMock> create(std::deque<GetNextResult> results);

    static boost::intrusive_ptr<DocumentSourceMock> create(const char* json);
    static boost::intrusive_ptr<DocumentSourceMock> create(
        const std::initializer_list<const char*>& jsons);

    void reattachToOperationContext(OperationContext* opCtx) {
        isDetachedFromOpCtx = false;
    }

    void detachFromOperationContext() {
        isDetachedFromOpCtx = true;
    }

    boost::intrusive_ptr<DocumentSource> optimize() override {
        isOptimized = true;
        return this;
    }

    // Return documents from front of queue.
    std::deque<GetNextResult> queue;

    bool isDisposed = false;
    bool isDetachedFromOpCtx = false;
    bool isOptimized = false;
    bool isExpCtxInjected = false;

    BSONObjSet sorts;

protected:
    void doDispose() override;
};

}  // namespace mongo
