/**
 * Copyright (c) 2011 10gen Inc.
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
 */

#include "pch.h"
#include "db/commands/pipeline.h"

#include "db/cursor.h"
#include "db/pipeline/accumulator.h"
#include "db/pipeline/dependency_tracker.h"
#include "db/pipeline/document.h"
#include "db/pipeline/document_source.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"
#include "db/pdfile.h"
#include "util/mongoutils/str.h"

namespace mongo {

    const char Pipeline::commandName[] = "aggregate";
    const char Pipeline::pipelineName[] = "pipeline";
    const char Pipeline::fromRouterName[] = "fromRouter";
    const char Pipeline::splitMongodPipelineName[] = "splitMongodPipeline";

    Pipeline::~Pipeline() {
    }

    Pipeline::Pipeline(const intrusive_ptr<ExpressionContext> &pTheCtx):
        collectionName(),
        sourceVector(),
        splitMongodPipeline(DEBUG_BUILD == 1), /* test: always split for DEV */
        pCtx(pTheCtx) {
    }



    /* this structure is used to make a lookup table of operators */
    struct StageDesc {
        const char *pName;
        intrusive_ptr<DocumentSource> (*pFactory)(
            BSONElement *, const intrusive_ptr<ExpressionContext> &);
    };

    /* this table must be in alphabetical order by name for bsearch() */
    static const StageDesc stageDesc[] = {
#ifdef NEVER /* disabled for now in favor of $match */
        {DocumentSourceFilter::filterName,
         DocumentSourceFilter::createFromBson},
#endif
        {DocumentSourceGroup::groupName,
         DocumentSourceGroup::createFromBson},
        {DocumentSourceLimit::limitName,
         DocumentSourceLimit::createFromBson},
        {DocumentSourceMatch::matchName,
         DocumentSourceMatch::createFromBson},
#ifdef LATER /* https://jira.mongodb.org/browse/SERVER-3253 */
        {DocumentSourceOut::outName,
         DocumentSourceOut::createFromBson},
#endif
        {DocumentSourceProject::projectName,
         DocumentSourceProject::createFromBson},
        {DocumentSourceSkip::skipName,
         DocumentSourceSkip::createFromBson},
        {DocumentSourceSort::sortName,
         DocumentSourceSort::createFromBson},
        {DocumentSourceUnwind::unwindName,
         DocumentSourceUnwind::createFromBson},
    };
    static const size_t nStageDesc = sizeof(stageDesc) / sizeof(StageDesc);

    static int stageDescCmp(const void *pL, const void *pR) {
        return strcmp(((const StageDesc *)pL)->pName,
                      ((const StageDesc *)pR)->pName);
    }

    intrusive_ptr<Pipeline> Pipeline::parseCommand(
        string &errmsg, BSONObj &cmdObj,
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<Pipeline> pPipeline(new Pipeline(pCtx));
        vector<BSONElement> pipeline;

        /* gather the specification for the aggregation */
        for(BSONObj::iterator cmdIterator = cmdObj.begin();
                cmdIterator.more(); ) {
            BSONElement cmdElement(cmdIterator.next());
            const char *pFieldName = cmdElement.fieldName();

            /* look for the aggregation command */
            if (!strcmp(pFieldName, commandName)) {
                pPipeline->collectionName = cmdElement.String();
                continue;
            }

            /* check for the collection name */
            if (!strcmp(pFieldName, pipelineName)) {
                pipeline = cmdElement.Array();
                continue;
            }

            /* if the request came from the router, we're in a shard */
            if (!strcmp(pFieldName, fromRouterName)) {
                pCtx->setInShard(cmdElement.Bool());
                continue;
            }

            /* check for debug options */
            if (!strcmp(pFieldName, splitMongodPipelineName)) {
                pPipeline->splitMongodPipeline = true;
                continue;
            }

            /* we didn't recognize a field in the command */
            ostringstream sb;
            sb <<
               "Pipeline::parseCommand(): unrecognized field \"" <<
               cmdElement.fieldName();
            errmsg = sb.str();
            return intrusive_ptr<Pipeline>();
        }

        /*
          If we get here, we've harvested the fields we expect for a pipeline.

          Set up the specified document source pipeline.
        */
        SourceVector *pSourceVector = &pPipeline->sourceVector; // shorthand

        /* iterate over the steps in the pipeline */
        const size_t nSteps = pipeline.size();
        for(size_t iStep = 0; iStep < nSteps; ++iStep) {
            /* pull out the pipeline element as an object */
            BSONElement pipeElement(pipeline[iStep]);
            uassert(15942, str::stream() << "pipeline element " <<
                    iStep << " is not an object",
                    pipeElement.type() == Object);
            BSONObj bsonObj(pipeElement.Obj());

            intrusive_ptr<DocumentSource> pSource;

            /* use the object to add a DocumentSource to the processing chain */
            BSONObjIterator bsonIterator(bsonObj);
            while(bsonIterator.more()) {
                BSONElement bsonElement(bsonIterator.next());
                const char *pFieldName = bsonElement.fieldName();

                /* select the appropriate operation and instantiate */
                StageDesc key;
                key.pName = pFieldName;
                const StageDesc *pDesc = (const StageDesc *)
                    bsearch(&key, stageDesc, nStageDesc, sizeof(StageDesc),
                            stageDescCmp);
                if (pDesc) {
                    pSource = (*pDesc->pFactory)(&bsonElement, pCtx);
                    pSource->setPipelineStep(iStep);
                }
                else {
                    ostringstream sb;
                    sb <<
                       "Pipeline::run(): unrecognized pipeline op \"" <<
                       pFieldName;
                    errmsg = sb.str();
                    return intrusive_ptr<Pipeline>();
                }
            }

            pSourceVector->push_back(pSource);
        }

        /* if there aren't any pipeline stages, there's nothing more to do */
        if (!pSourceVector->size())
            return pPipeline;

        /*
          Move filters up where possible.

          CW TODO -- move filter past projections where possible, and noting
          corresponding field renaming.
        */

        /*
          Wherever there is a match immediately following a sort, swap them.
          This means we sort fewer items.  Neither changes the documents in
          the stream, so this transformation shouldn't affect the result.

          We do this first, because then when we coalesce operators below,
          any adjacent matches will be combined.
         */
        for(size_t srcn = pSourceVector->size(), srci = 1;
            srci < srcn; ++srci) {
            intrusive_ptr<DocumentSource> &pSource = pSourceVector->at(srci);
            if (dynamic_cast<DocumentSourceMatch *>(pSource.get())) {
                intrusive_ptr<DocumentSource> &pPrevious =
                    pSourceVector->at(srci - 1);
                if (dynamic_cast<DocumentSourceSort *>(pPrevious.get())) {
                    /* swap this item with the previous */
                    intrusive_ptr<DocumentSource> pTemp(pPrevious);
                    pPrevious = pSource;
                    pSource = pTemp;
                }
            }
        }

        /*
          Coalesce adjacent filters where possible.  Two adjacent filters
          are equivalent to one filter whose predicate is the conjunction of
          the two original filters' predicates.  For now, capture this by
          giving any DocumentSource the option to absorb it's successor; this
          will also allow adjacent projections to coalesce when possible.

          Run through the DocumentSources, and give each one the opportunity
          to coalesce with its successor.  If successful, remove the
          successor.

          Move all document sources to a temporary list.
        */
        SourceVector tempVector(*pSourceVector);
        pSourceVector->clear();

        /* move the first one to the final list */
        pSourceVector->push_back(tempVector[0]);

        /* run through the sources, coalescing them or keeping them */
        for(size_t tempn = tempVector.size(), tempi = 1;
            tempi < tempn; ++tempi) {
            /*
              If we can't coalesce the source with the last, then move it
              to the final list, and make it the new last.  (If we succeeded,
              then we're still on the same last, and there's no need to move
              or do anything with the source -- the destruction of tempVector
              will take care of the rest.)
            */
            intrusive_ptr<DocumentSource> &pLastSource = pSourceVector->back();
            intrusive_ptr<DocumentSource> &pTemp = tempVector.at(tempi);
            if (!pLastSource->coalesce(pTemp))
                pSourceVector->push_back(pTemp);
        }

        /* optimize the elements in the pipeline */
        for(SourceVector::iterator iter(pSourceVector->begin()),
                listEnd(pSourceVector->end()); iter != listEnd; ++iter)
            (*iter)->optimize();

        return pPipeline;
    }

    intrusive_ptr<Pipeline> Pipeline::splitForSharded() {
        /* create an initialize the shard spec we'll return */
        intrusive_ptr<Pipeline> pShardPipeline(new Pipeline(pCtx));
        pShardPipeline->collectionName = collectionName;

        /* put the source list aside */
        SourceVector tempVector(sourceVector);
        sourceVector.clear();

        /*
          Run through the pipeline, looking for points to split it into
          shard pipelines, and the rest.
         */
        while(!tempVector.empty()) {
            intrusive_ptr<DocumentSource> &pSource = tempVector.front();

            /* hang on to this in advance, in case it is a group */
            DocumentSourceGroup *pGroup =
                dynamic_cast<DocumentSourceGroup *>(pSource.get());

            /* move the source from the tempVector to the shard sourceVector */
            pShardPipeline->sourceVector.push_back(pSource);
            tempVector.erase(tempVector.begin());

            /*
              If we found a group, that's a split point.
             */
            if (pGroup) {
                /* start this pipeline with the group merger */
                sourceVector.push_back(pGroup->createMerger());

                /* and then add everything that remains and quit */
                for(size_t tempn = tempVector.size(), tempi = 0;
                    tempi < tempn; ++tempi)
                    sourceVector.push_back(tempVector[tempi]);
                break;
            }
        }

        return pShardPipeline;
    }

    bool Pipeline::getInitialQuery(BSONObjBuilder *pQueryBuilder) const
    {
        if (!sourceVector.size())
            return false;

        /* look for an initial $match */
        const intrusive_ptr<DocumentSource> &pMC = sourceVector.front();
        const DocumentSourceMatch *pMatch =
            dynamic_cast<DocumentSourceMatch *>(pMC.get());

        if (!pMatch)
            return false;

        /* build the query */
        pMatch->toMatcherBson(pQueryBuilder);

        return true;
    }

    void Pipeline::toBson(BSONObjBuilder *pBuilder) const {
        /* create an array out of the pipeline operations */
        BSONArrayBuilder arrayBuilder;
        for(SourceVector::const_iterator iter(sourceVector.begin()),
                listEnd(sourceVector.end()); iter != listEnd; ++iter) {
            intrusive_ptr<DocumentSource> pSource(*iter);
            pSource->addToBsonArray(&arrayBuilder);
        }

        /* add the top-level items to the command */
        pBuilder->append(commandName, getCollectionName());
        pBuilder->append(pipelineName, arrayBuilder.arr());

        bool btemp;
        if ((btemp = getSplitMongodPipeline())) {
            pBuilder->append(splitMongodPipelineName, btemp);
        }
        if ((btemp = pCtx->getInRouter())) {
            pBuilder->append(fromRouterName, btemp);
        }
    }

    bool Pipeline::run(BSONObjBuilder &result, string &errmsg,
                       const intrusive_ptr<DocumentSource> &pInputSource) {
        /*
          Analyze dependency information.

          This pushes dependencies from the end of the pipeline back to the
          front of it, and finally passes that to the input source before we
          execute the pipeline.
        */
        intrusive_ptr<DependencyTracker> pTracker(new DependencyTracker());
        for(SourceVector::reverse_iterator iter(sourceVector.rbegin()),
                listBeg(sourceVector.rend()); iter != listBeg; ++iter) {
            intrusive_ptr<DocumentSource> pTemp(*iter);
            pTemp->manageDependencies(pTracker);
        }

        pInputSource->manageDependencies(pTracker);
        
        /* chain together the sources we found */
        DocumentSource *pSource = pInputSource.get();
        for(SourceVector::iterator iter(sourceVector.begin()),
                listEnd(sourceVector.end()); iter != listEnd; ++iter) {
            intrusive_ptr<DocumentSource> pTemp(*iter);
            pTemp->setSource(pSource);
            pSource = pTemp.get();
        }
        /* pSource is left pointing at the last source in the chain */

        /*
          Iterate through the resulting documents, and add them to the result.

          We wrap all the BSONObjBuilder calls with a try/catch in case the
          objects get too large and cause an exception.
        */
        try {
            BSONArrayBuilder resultArray; // where we'll stash the results
            for(bool hasDocument = !pSource->eof(); hasDocument;
                hasDocument = pSource->advance()) {
                boost::intrusive_ptr<Document> pDocument(pSource->getCurrent());

                /* add the document to the result set */
                BSONObjBuilder documentBuilder;
                pDocument->toBson(&documentBuilder);
                resultArray.append(documentBuilder.done());
            }

            result.appendArray("result", resultArray.arr());

         } catch(AssertionException &ae) {
            /* 
               If its not the "object too large" error, rethrow.
               At time of writing, that error code comes from
               mongo/src/mongo/bson/util/builder.h
            */
            if (ae.getCode() != 13548)
                throw;

            /* throw the nicer human-readable error */
            uassert(16029, str::stream() <<
                    "aggregation result exceeds maximum document size limit ("
                    << (BSONObjMaxUserSize / (1024 * 1024)) << "MB)",
                    false);
         }

        return true;
    }

} // namespace mongo
