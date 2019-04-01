/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

namespace mongo {

class DocumentSourceMergeCursors;
class DocumentSourceSequentialDocumentCache;
class DocumentSourceSample;
class DocumentSourceUnwind;
class DocumentSourceCloseCursor;
class DocumentSourceGeoNear;
class DocumentSourceLookUp;
class DocumentSourceInternalInhibitOptimization;
class DocumentSourceLookupChangePostImage;
class DocumentSourceListCachedAndActiveUsers;
class DocumentSourceChangeStreamTransform;
class DocumentSourceOut;
class DocumentSourceRedact;
class DocumentSourceGraphLookUp;
class DocumentSourceExchange;
class DocumentSourceCursor;
class DocumentSourceBucketAuto;
class DocumentSourceSkip;
class DocumentSourceSort;
class DocumentSourceGroup;
class DocumentSourceMock;
class DocumentSourceListLocalSessions;
class DocumentSourcePlanCacheStats;
class DocumentSourceIndexStats;
class DocumentSourceLimit;
class DocumentSourceCurrentOp;
class DocumentSourceSingleDocumentTransformation;
class DocumentSourceChangeStream;
class DocumentSourceShardCheckResumability;
class DocumentSourceEnsureResumeTokenPresent;
class DocumentSourceCheckInvalidate;
class DocumentSourceFacet;
class DocumentSourceInternalSplitPipeline;
class DocumentSourceListSessions;
class DocumentSourceCollStats;
class DocumentSourceTeeConsumer;
class DocumentSourceSampleFromRandomCursor;
class DocumentSourceMatch;
class DocumentSourceOplogMatch;
class DocumentSourceUpdateOnAddShard;
class DocumentSourceBackupCursorExtend;
class DocumentSourceBackupCursor;
class DocumentSourceTestOptimizations;

/**
 * This is a base class to allow for dynamic dispatch on a DocumentSource. It implements the visitor
 * pattern, in which every derived class from DocumentSource implements an acceptVisitor() method,
 * which simply calls the appropriate visit() method on the derived DocumentSourceVisitor class. The
 * derived class can do whatever it needs to do for each specific node type in the corresponding
 * visit() method.
 */
class DocumentSourceVisitor {
public:
    virtual ~DocumentSourceVisitor() = default;

    virtual void visit(DocumentSourceMergeCursors* source) = 0;
    virtual void visit(DocumentSourceSequentialDocumentCache* source) = 0;
    virtual void visit(DocumentSourceSample* source) = 0;
    virtual void visit(DocumentSourceUnwind* source) = 0;
    virtual void visit(DocumentSourceCloseCursor* source) = 0;
    virtual void visit(DocumentSourceGeoNear* source) = 0;
    virtual void visit(DocumentSourceLookUp* source) = 0;
    virtual void visit(DocumentSourceInternalInhibitOptimization* source) = 0;
    virtual void visit(DocumentSourceLookupChangePostImage* source) = 0;
    virtual void visit(DocumentSourceListCachedAndActiveUsers* source) = 0;
    virtual void visit(DocumentSourceChangeStreamTransform* source) = 0;
    virtual void visit(DocumentSourceOut* source) = 0;
    virtual void visit(DocumentSourceRedact* source) = 0;
    virtual void visit(DocumentSourceGraphLookUp* source) = 0;
    virtual void visit(DocumentSourceExchange* source) = 0;
    virtual void visit(DocumentSourceCursor* source) = 0;
    virtual void visit(DocumentSourceBucketAuto* source) = 0;
    virtual void visit(DocumentSourceSkip* source) = 0;
    virtual void visit(DocumentSourceSort* source) = 0;
    virtual void visit(DocumentSourceGroup* source) = 0;
    virtual void visit(DocumentSourceMock* source) = 0;
    virtual void visit(DocumentSourceListLocalSessions* source) = 0;
    virtual void visit(DocumentSourcePlanCacheStats* source) = 0;
    virtual void visit(DocumentSourceIndexStats* source) = 0;
    virtual void visit(DocumentSourceLimit* source) = 0;
    virtual void visit(DocumentSourceCurrentOp* source) = 0;
    virtual void visit(DocumentSourceSingleDocumentTransformation* source) = 0;
    virtual void visit(DocumentSourceOplogMatch* source) = 0;
    virtual void visit(DocumentSourceShardCheckResumability* source) = 0;
    virtual void visit(DocumentSourceEnsureResumeTokenPresent* source) = 0;
    virtual void visit(DocumentSourceCheckInvalidate* source) = 0;
    virtual void visit(DocumentSourceFacet* source) = 0;
    virtual void visit(DocumentSourceInternalSplitPipeline* source) = 0;
    virtual void visit(DocumentSourceListSessions* source) = 0;
    virtual void visit(DocumentSourceCollStats* source) = 0;
    virtual void visit(DocumentSourceTeeConsumer* source) = 0;
    virtual void visit(DocumentSourceSampleFromRandomCursor* source) = 0;
    virtual void visit(DocumentSourceMatch* source) = 0;
    virtual void visit(DocumentSourceUpdateOnAddShard* source) = 0;
    virtual void visit(DocumentSourceBackupCursorExtend* source) = 0;
    virtual void visit(DocumentSourceBackupCursor* source) = 0;
    virtual void visit(DocumentSourceTestOptimizations* source) = 0;
};

}  // namespace mongo
