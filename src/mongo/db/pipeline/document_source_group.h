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

#include <memory>
#include <utility>

#include "mongo/db/pipeline/document_source_group_base.h"

namespace mongo {

/**
 * This class represents hash based group implementation that stores all groups until source is
 * depleted and only then starts outputing documents.
 */
class DocumentSourceGroup final : public DocumentSourceGroupBase {
public:
    static constexpr StringData kStageName = "$group"_sd;

    const char* getSourceName() const final;

    /**
     * Convenience method for creating a new $group stage. If maxMemoryUsageBytes is boost::none,
     * then it will actually use the value of internalDocumentSourceGroupMaxMemoryBytes.
     */
    static boost::intrusive_ptr<DocumentSourceGroup> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        std::vector<AccumulationStatement> accumulationStatements,
        boost::optional<size_t> maxMemoryUsageBytes = boost::none);

    /**
     * Parses 'elem' into a $group stage, or throws a AssertionException if 'elem' was an invalid
     * specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    static boost::intrusive_ptr<DocumentSource> createFromBsonWithMaxMemoryUsage(
        BSONElement elem,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<size_t> maxMemoryUsageBytes);

protected:
    GetNextResult doGetNext() final;

    bool isSpecFieldReserved(StringData) final {
        return false;
    }

private:
    explicit DocumentSourceGroup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 boost::optional<size_t> maxMemoryUsageBytes = boost::none);

    /**
     * Before returning anything, this source must prepare itself. performBlockingGroup() exhausts
     * the previous source before
     * returning. The '_groupsReady' boolean indicates that performBlockingGroup() has finished.
     *
     * This method may not be able to finish initialization in a single call if 'pSource' returns a
     * DocumentSource::GetNextResult::kPauseExecution, so it returns the last GetNextResult
     * encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult performBlockingGroup();

    /**
     * Initializes this $group after any children are initialized. See performBlockingGroup() for
     * more details.
     */
    GetNextResult performBlockingGroupSelf(GetNextResult input);

    bool _groupsReady;
};

}  // namespace mongo
