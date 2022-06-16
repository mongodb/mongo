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

#pragma once

#include "mongo/db/query/tree_walker.h"

namespace mongo {

class DocumentSourceBucketAuto;
class DocumentSourceCollStats;
class DocumentSourceCurrentOp;
class DocumentSourceCursor;
class DocumentSourceExchange;
class DocumentSourceFacet;
class DocumentSourceGeoNear;
class DocumentSourceGeoNearCursor;
class DocumentSourceGraphLookUp;
class DocumentSourceGroup;
class DocumentSourceIndexStats;
class DocumentSourceInternalInhibitOptimization;
class DocumentSourceInternalShardFilter;
class DocumentSourceInternalSplitPipeline;
class DocumentSourceInternalUnpackBucket;
class DocumentSourceLimit;
class DocumentSourceListCachedAndActiveUsers;
class DocumentSourceListLocalSessions;
class DocumentSourceListSessions;
class DocumentSourceLookUp;
class DocumentSourceMatch;
class DocumentSourceMerge;
class DocumentSourceMergeCursors;
class DocumentSourceOperationMetrics;
class DocumentSourceOut;
class DocumentSourcePlanCacheStats;
class DocumentSourceQueue;
class DocumentSourceRedact;
class DocumentSourceSample;
class DocumentSourceSampleFromRandomCursor;
class DocumentSourceSequentialDocumentCache;
class DocumentSourceSingleDocumentTransformation;
class DocumentSourceSkip;
class DocumentSourceSort;
class DocumentSourceTeeConsumer;
class DocumentSourceUnionWith;
class DocumentSourceUnwind;

/**
 * Visitor pattern for pipeline document sources.
 *
 * This code is not responsible for traversing the tree, only for performing the double-dispatch.
 *
 * If the visitor doesn't intend to modify the tree, then the template argument 'IsConst' should be
 * set to 'true'. In this case all 'visit()' methods will take a const pointer to a visiting node.
 */
template <bool IsConst = false>
class DocumentSourceVisitor {
public:
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceBucketAuto> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceCollStats> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceCurrentOp> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceCursor> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceExchange> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceFacet> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceGeoNear> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceGeoNearCursor> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceGraphLookUp> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceGroup> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceIndexStats> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceInternalInhibitOptimization> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceInternalShardFilter> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceInternalSplitPipeline> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceInternalUnpackBucket> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceLimit> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceListCachedAndActiveUsers> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceListLocalSessions> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceListSessions> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceLookUp> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceMatch> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceMerge> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceMergeCursors> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceOperationMetrics> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceOut> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourcePlanCacheStats> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceQueue> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceRedact> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceSample> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceSampleFromRandomCursor> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceSequentialDocumentCache> source) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceSingleDocumentTransformation> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceSkip> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceSort> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceTeeConsumer> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceUnionWith> source) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceUnwind> source) = 0;
};

using DocumentSourceMutableVisitor = DocumentSourceVisitor<false>;
using DocumentSourceConstVisitor = DocumentSourceVisitor<true>;

}  // namespace mongo
