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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/pipeline.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    const char Pipeline::commandName[] = "aggregate";
    const char Pipeline::pipelineName[] = "pipeline";
    const char Pipeline::explainName[] = "explain";
    const char Pipeline::fromRouterName[] = "fromRouter";
    const char Pipeline::splitMongodPipelineName[] = "splitMongodPipeline";
    const char Pipeline::serverPipelineName[] = "serverPipeline";
    const char Pipeline::mongosPipelineName[] = "mongosPipeline";

    Pipeline::Pipeline(const intrusive_ptr<ExpressionContext> &pTheCtx):
        collectionName(),
        explain(false),
        splitMongodPipeline(false),
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
        {DocumentSourceGeoNear::geoNearName,
         DocumentSourceGeoNear::createFromBson},
        {DocumentSourceGroup::groupName,
         DocumentSourceGroup::createFromBson},
        {DocumentSourceLimit::limitName,
         DocumentSourceLimit::createFromBson},
        {DocumentSourceMatch::matchName,
         DocumentSourceMatch::createFromBson},
        {DocumentSourceMergeCursors::name,
         DocumentSourceMergeCursors::createFromBson},
        {DocumentSourceOut::outName,
         DocumentSourceOut::createFromBson},
        {DocumentSourceProject::projectName,
         DocumentSourceProject::createFromBson},
        {DocumentSourceRedact::redactName,
         DocumentSourceRedact::createFromBson},
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

            // ignore top-level fields prefixed with $. They are for the command processor, not us.
            if (pFieldName[0] == '$') {
                continue;
            }

            // ignore cursor options since they are handled externally.
            if (str::equals(pFieldName, "cursor")) {
                continue;
            }

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

            /* check for explain option */
            if (!strcmp(pFieldName, explainName)) {
                pPipeline->explain = cmdElement.Bool();
                continue;
            }

            /* if the request came from the router, we're in a shard */
            if (!strcmp(pFieldName, fromRouterName)) {
                pCtx->inShard = cmdElement.Bool();
                continue;
            }

            /* check for debug options */
            if (!strcmp(pFieldName, splitMongodPipelineName)) {
                pPipeline->splitMongodPipeline = true;
                continue;
            }

            if (str::equals(pFieldName, "allowDiskUsage")) {
                uassert(16949,
                        str::stream() << "allowDiskUsage must be a bool, not a "
                                      << typeName(cmdElement.type()),
                        cmdElement.type() == Bool);
                pCtx->extSortAllowed = cmdElement.Bool();
                continue;
            }

            /* we didn't recognize a field in the command */
            ostringstream sb;
            sb << "unrecognized field '" << cmdElement.fieldName() << "'";
            errmsg = sb.str();
            return intrusive_ptr<Pipeline>();
        }

        /*
          If we get here, we've harvested the fields we expect for a pipeline.

          Set up the specified document source pipeline.
        */
        SourceContainer& sources = pPipeline->sources; // shorthand

        /* iterate over the steps in the pipeline */
        const size_t nSteps = pipeline.size();
        for(size_t iStep = 0; iStep < nSteps; ++iStep) {
            /* pull out the pipeline element as an object */
            BSONElement pipeElement(pipeline[iStep]);
            uassert(15942, str::stream() << "pipeline element " <<
                    iStep << " is not an object",
                    pipeElement.type() == Object);
            BSONObj bsonObj(pipeElement.Obj());

            // Parse a pipeline stage from 'bsonObj'.
            uassert(16435, "A pipeline stage specification object must contain exactly one field.",
                    bsonObj.nFields() == 1);
            BSONElement stageSpec = bsonObj.firstElement();
            const char* stageName = stageSpec.fieldName();

            // Create a DocumentSource pipeline stage from 'stageSpec'.
            StageDesc key;
            key.pName = stageName;
            const StageDesc* pDesc = (const StageDesc*)
                    bsearch(&key, stageDesc, nStageDesc, sizeof(StageDesc),
                            stageDescCmp);

            uassert(16436,
                    str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
                    pDesc);
            intrusive_ptr<DocumentSource> stage = (*pDesc->pFactory)(&stageSpec, pCtx);
            verify(stage);
            stage->setPipelineStep(iStep);
            sources.push_back(stage);

            if (dynamic_cast<DocumentSourceOut*>(stage.get())) {
                uassert(16991, "$out can only be the final stage in the pipeline",
                        iStep == nSteps - 1);
            }
        }

        /* if there aren't any pipeline stages, there's nothing more to do */
        if (sources.empty())
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
        for (size_t srcn = sources.size(), srci = 1; srci < srcn; ++srci) {
            intrusive_ptr<DocumentSource> &pSource = sources[srci];
            if (dynamic_cast<DocumentSourceMatch *>(pSource.get())) {
                intrusive_ptr<DocumentSource> &pPrevious = sources[srci - 1];
                if (dynamic_cast<DocumentSourceSort *>(pPrevious.get())) {
                    /* swap this item with the previous */
                    intrusive_ptr<DocumentSource> pTemp(pPrevious);
                    pPrevious = pSource;
                    pSource = pTemp;
                }
            }
        }

        /* Move limits in front of skips. This is more optimal for sharding
         * since currently, we can only split the pipeline at a single source
         * and it is better to limit the results coming from each shard
         */
        for(int i = sources.size() - 1; i >= 1 /* not looking at 0 */; i--) {
            DocumentSourceLimit* limit =
                dynamic_cast<DocumentSourceLimit*>(sources[i].get());
            DocumentSourceSkip* skip =
                dynamic_cast<DocumentSourceSkip*>(sources[i-1].get());
            if (limit && skip) {
                // Increase limit by skip since the skipped docs now pass through the $limit
                limit->setLimit(limit->getLimit() + skip->getSkip());
                swap(sources[i], sources[i-1]);

                // Start at back again. This is needed to handle cases with more than 1 $limit
                // (S means skip, L means limit)
                //
                // These two would work without second pass (assuming back to front ordering)
                // SL   -> LS
                // SSL  -> LSS
                //
                // The following cases need a second pass to handle the second limit
                // SLL  -> LLS
                // SSLL -> LLSS
                // SLSL -> LLSS
                i = sources.size(); // decremented before next pass
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
        SourceContainer tempSources;
        sources.swap(tempSources);

        /* move the first one to the final list */
        sources.push_back(tempSources[0]);

        /* run through the sources, coalescing them or keeping them */
        for (size_t tempn = tempSources.size(), tempi = 1; tempi < tempn; ++tempi) {
            /*
              If we can't coalesce the source with the last, then move it
              to the final list, and make it the new last.  (If we succeeded,
              then we're still on the same last, and there's no need to move
              or do anything with the source -- the destruction of tempSources
              will take care of the rest.)
            */
            intrusive_ptr<DocumentSource> &pLastSource = sources.back();
            intrusive_ptr<DocumentSource> &pTemp = tempSources[tempi];
            verify(pTemp && pLastSource);
            if (!pLastSource->coalesce(pTemp))
                sources.push_back(pTemp);
        }

        /* optimize the elements in the pipeline */
        for(SourceContainer::iterator iter(sources.begin()),
                                      listEnd(sources.end());
                                    iter != listEnd;
                                    ++iter) {
            if (!*iter) {
                errmsg = "Pipeline received empty document as argument";
                return intrusive_ptr<Pipeline>();
            }

            (*iter)->optimize();
        }

        return pPipeline;
    }

    void Pipeline::addRequiredPrivileges(const string& db,
                                         BSONObj cmdObj,
                                         vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(db + '.' + cmdObj.firstElement().str(), actions));

        if (false && cmdObj["allowDiskUsage"].trueValue()) {
            // TODO no privilege for this yet.
        }

        BSONObj pipeline = cmdObj.getObjectField("pipeline");
        BSONForEach(stageElem, pipeline) {
            BSONObj stage = stageElem.embeddedObjectUserCheck();
            if (str::equals(stage.firstElementFieldName(), "$out")) {
                // TODO Figure out how to handle temp collection privileges. For now, using the
                // output ns is ok since we only do db-level privilege checks.
                const string outputNs = db + '.' + stage.firstElement().str();

                ActionSet actions;
                // logically on output ns
                actions.addAction(ActionType::remove);
                actions.addAction(ActionType::insert);
                actions.addAction(ActionType::indexRead);

                // on temp ns due to implementation, but not logically on output ns
                actions.addAction(ActionType::createCollection);
                actions.addAction(ActionType::ensureIndex);
                actions.addAction(ActionType::dropCollection);
                actions.addAction(ActionType::renameCollectionSameDB);

                out->push_back(Privilege(outputNs, actions));
            }
        }
    }

    intrusive_ptr<Pipeline> Pipeline::splitForSharded() {
        /* create an initialize the shard spec we'll return */
        intrusive_ptr<Pipeline> pShardPipeline(new Pipeline(pCtx));
        pShardPipeline->collectionName = collectionName;
        pShardPipeline->explain = explain;

        /*
          Run through the pipeline, looking for points to split it into
          shard pipelines, and the rest.
         */
        while (!sources.empty()) {
            // pop the first source
            intrusive_ptr<DocumentSource> pSource = sources.front();
            sources.pop_front();

            // Check if this source is splittable
            SplittableDocumentSource* splittable=
                dynamic_cast<SplittableDocumentSource *>(pSource.get());

            if (!splittable){
                // move the source from the router sources to the shard sources
                pShardPipeline->sources.push_back(pSource);
            }
            else {
                // split into Router and Shard sources
                intrusive_ptr<DocumentSource> shardSource  = splittable->getShardSource();
                intrusive_ptr<DocumentSource> routerSource = splittable->getRouterSource();
                if (shardSource) pShardPipeline->sources.push_back(shardSource);
                if (routerSource)          this->sources.push_front(routerSource);

                break;
            }
        }

        return pShardPipeline;
    }

    bool Pipeline::getInitialQuery(BSONObjBuilder *pQueryBuilder) const
    {
        if (sources.empty())
            return false;

        /* look for an initial $match */
        const intrusive_ptr<DocumentSource> &pMC = sources.front();
        const DocumentSourceMatch *pMatch =
            dynamic_cast<DocumentSourceMatch *>(pMC.get());

        if (!pMatch)
            return false;

        /* build the query */
        pMatch->toMatcherBson(pQueryBuilder);

        return true;
    }

    Document Pipeline::serialize() const {
        MutableDocument serialized;
        // create an array out of the pipeline operations
        vector<Value> array;
        for(SourceContainer::const_iterator iter(sources.begin()),
                                            listEnd(sources.end());
                                        iter != listEnd;
                                        ++iter) {
            intrusive_ptr<DocumentSource> pSource(*iter);
            pSource->serializeToArray(array);
        }

        // add the top-level items to the command
        serialized.setField(commandName, Value(getCollectionName()));
        serialized.setField(pipelineName, Value(array));

        if (explain) {
            serialized.setField(explainName, Value(explain));
        }

        if (pCtx->extSortAllowed) {
            serialized.setField("allowDiskUsage", Value(true));
        }

        bool btemp;
        if ((btemp = getSplitMongodPipeline())) {
            serialized.setField(splitMongodPipelineName, Value(btemp));
        }

        return serialized.freeze();
    }

    void Pipeline::stitch() {
        massert(16600, "should not have an empty pipeline",
                !sources.empty());

        /* chain together the sources we found */
        DocumentSource* prevSource = sources.front().get();
        for(SourceContainer::iterator iter(sources.begin() + 1),
                                      listEnd(sources.end());
                                    iter != listEnd;
                                    ++iter) {
            intrusive_ptr<DocumentSource> pTemp(*iter);
            pTemp->setSource(prevSource);
            prevSource = pTemp.get();
        }
    }

    void Pipeline::run(BSONObjBuilder& result) {
        /*
          Iterate through the resulting documents, and add them to the result.
          We do this even if we're doing an explain, in order to capture
          the document counts and other stats.  However, we don't capture
          the result documents for explain.
        */
        if (explain) {
            if (!pCtx->inRouter)
                writeExplainShard(result);
            else {
                writeExplainMongos(result);
            }
        }
        else {
            // the array in which the aggregation results reside
            // cant use subArrayStart() due to error handling
            BSONArrayBuilder resultArray;
            DocumentSource* finalSource = sources.back().get();
            while (boost::optional<Document> next = finalSource->getNext()) {
                /* add the document to the result set */
                BSONObjBuilder documentBuilder (resultArray.subobjStart());
                next->toBson(&documentBuilder);
                documentBuilder.doneFast();
                // object will be too large, assert. the extra 1KB is for headers
                uassert(16389,
                        str::stream() << "aggregation result exceeds maximum document size ("
                                      << BSONObjMaxUserSize / (1024 * 1024) << "MB)",
                        resultArray.len() < BSONObjMaxUserSize - 1024);
            }

            resultArray.done();
            result.appendArray("result", resultArray.arr());
        }
    }

    vector<Value> Pipeline::writeExplainOps() const {
        vector<Value> array;
        for(SourceContainer::const_iterator iter(sources.begin()),
                                            listEnd(sources.end());
                                        iter != listEnd;
                                        ++iter) {
            intrusive_ptr<DocumentSource> pSource(*iter);

            // handled in writeExplainMongos
            if (dynamic_cast<DocumentSourceBsonArray*>(pSource.get()))
                continue;

            pSource->serializeToArray(array, true);
        }
        return array;
    }

    void Pipeline::writeExplainShard(BSONObjBuilder &result) const {
        result << serverPipelineName << Value(writeExplainOps());
    }

    void Pipeline::writeExplainMongos(BSONObjBuilder &result) const {

        /*
          For now, this should be a BSON source array.
          In future, we might have a more clever way of getting this, when
          we have more interleaved fetching between shards.  The DocumentSource
          interface will have to change to accommodate that.
         */
        DocumentSourceBsonArray *pSourceBsonArray =
            dynamic_cast<DocumentSourceBsonArray *>(sources.front().get());
        verify(pSourceBsonArray);

        BSONArrayBuilder shardOpArray; // where we'll put the pipeline ops
        while (boost::optional<Document> next = pSourceBsonArray->getNext()) {
            BSONObjBuilder opBuilder(shardOpArray.subobjStart());
            next->toBson(&opBuilder);
            opBuilder.doneFast();
        }

        // now we combine the shard pipelines with the one here
        result.append(serverPipelineName, shardOpArray.arr());
        result << mongosPipelineName << Value(writeExplainOps());
    }

    void Pipeline::addInitialSource(intrusive_ptr<DocumentSource> source) {
        sources.push_front(source);
    }

    bool Pipeline::canRunInMongos() const {
        if (pCtx->extSortAllowed)
            return false;

        if (dynamic_cast<DocumentSourceNeedsMongod*>(sources.back().get()))
            return false;

        return true;
    }

} // namespace mongo
