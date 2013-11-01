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

// This file defines functions from both of these headers
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_optimizations.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    const char Pipeline::commandName[] = "aggregate";
    const char Pipeline::pipelineName[] = "pipeline";
    const char Pipeline::explainName[] = "explain";
    const char Pipeline::fromRouterName[] = "fromRouter";
    const char Pipeline::serverPipelineName[] = "serverPipeline";
    const char Pipeline::mongosPipelineName[] = "mongosPipeline";

    Pipeline::Pipeline(const intrusive_ptr<ExpressionContext> &pTheCtx):
        explain(false),
        pCtx(pTheCtx) {
    }


    /* this structure is used to make a lookup table of operators */
    struct StageDesc {
        const char *pName;
        intrusive_ptr<DocumentSource> (*pFactory)(
            BSONElement, const intrusive_ptr<ExpressionContext> &);
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

    intrusive_ptr<Pipeline> Pipeline::parseCommand(string &errmsg,
                                                   BSONObj &cmdObj,
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

            // maxTimeMS is also for the command processor.
            if (pFieldName == LiteParsedQuery::cmdOptionMaxTimeMS) {
                continue;
            }

            // ignore cursor options since they are handled externally.
            if (str::equals(pFieldName, "cursor")) {
                continue;
            }

            /* look for the aggregation command */
            if (!strcmp(pFieldName, commandName)) {
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
            intrusive_ptr<DocumentSource> stage = pDesc->pFactory(stageSpec, pCtx);
            verify(stage);
            sources.push_back(stage);

            if (dynamic_cast<DocumentSourceOut*>(stage.get())) {
                uassert(16991, "$out can only be the final stage in the pipeline",
                        iStep == nSteps - 1);
            }
        }

        // The order in which optimizations are applied can have significant impact on the
        // efficiency of the final pipeline. Be Careful!
        Optimizations::Local::moveMatchBeforeSort(pPipeline.get());
        Optimizations::Local::moveLimitBeforeSkip(pPipeline.get());
        Optimizations::Local::coalesceAdjacent(pPipeline.get());
        Optimizations::Local::optimizeEachDocumentSource(pPipeline.get());
        Optimizations::Local::duplicateMatchBeforeInitalRedact(pPipeline.get());

        return pPipeline;
    }

    void Pipeline::Optimizations::Local::moveMatchBeforeSort(Pipeline* pipeline) {
        SourceContainer& sources = pipeline->sources;
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
    }

    void Pipeline::Optimizations::Local::moveLimitBeforeSkip(Pipeline* pipeline) {
        SourceContainer& sources = pipeline->sources;
        if (sources.empty())
            return;

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
    }

    void Pipeline::Optimizations::Local::coalesceAdjacent(Pipeline* pipeline) {
        SourceContainer& sources = pipeline->sources;
        if (sources.empty())
            return;

        // move all sources to a temporary list
        SourceContainer tempSources;
        sources.swap(tempSources);

        // move the first one to the final list
        sources.push_back(tempSources[0]);

        // run through the sources, coalescing them or keeping them
        for (size_t tempn = tempSources.size(), tempi = 1; tempi < tempn; ++tempi) {
            // If we can't coalesce the source with the last, then move it
            // to the final list, and make it the new last.  (If we succeeded,
            // then we're still on the same last, and there's no need to move
            // or do anything with the source -- the destruction of tempSources
            // will take care of the rest.)
            intrusive_ptr<DocumentSource> &pLastSource = sources.back();
            intrusive_ptr<DocumentSource> &pTemp = tempSources[tempi];
            verify(pTemp && pLastSource);
            if (!pLastSource->coalesce(pTemp))
                sources.push_back(pTemp);
        }
    }

    void Pipeline::Optimizations::Local::optimizeEachDocumentSource(Pipeline* pipeline) {
        SourceContainer& sources = pipeline->sources;
        for (SourceContainer::iterator it(sources.begin()); it != sources.end(); ++it) {
            (*it)->optimize();
        }
    }

    void Pipeline::Optimizations::Local::duplicateMatchBeforeInitalRedact(Pipeline* pipeline) {
        SourceContainer& sources = pipeline->sources;
        if (sources.size() >= 2 && dynamic_cast<DocumentSourceRedact*>(sources[0].get())) {
            if (DocumentSourceMatch* match = dynamic_cast<DocumentSourceMatch*>(sources[1].get())) {
                const BSONObj redactSafePortion = match->redactSafePortion();
                if (!redactSafePortion.isEmpty()) {
                    sources.push_front(
                        DocumentSourceMatch::createFromBson(
                            BSON("$match" << redactSafePortion).firstElement(),
                            pipeline->pCtx));
                }
            }
        }
    }

    void Pipeline::addRequiredPrivileges(Command* commandTemplate,
                                         const string& db,
                                         BSONObj cmdObj,
                                         vector<Privilege>* out) {
        ResourcePattern inputResource(commandTemplate->parseResourcePattern(db, cmdObj));
        uassert(17138,
                mongoutils::str::stream() << "Invalid input resource, " << inputResource.toString(),
                inputResource.isExactNamespacePattern());

        if (false && cmdObj["allowDiskUsage"].trueValue()) {
            // TODO no privilege for this yet.
        }

        out->push_back(Privilege(inputResource, ActionType::find));

        BSONObj pipeline = cmdObj.getObjectField("pipeline");
        BSONForEach(stageElem, pipeline) {
            BSONObj stage = stageElem.embeddedObjectUserCheck();
            if (str::equals(stage.firstElementFieldName(), "$out")) {
                // TODO Figure out how to handle temp collection privileges. For now, using the
                // output ns is ok since we only do db-level privilege checks.
                NamespaceString outputNs(db, stage.firstElement().str());
                uassert(17139,
                        mongoutils::str::stream() << "Invalid $out target namespace, " <<
                        outputNs.ns(),
                        outputNs.isValid());

                ActionSet actions;
                // logically on output ns
                actions.addAction(ActionType::remove);
                actions.addAction(ActionType::insert);

                // on temp ns due to implementation, but not logically on output ns
                actions.addAction(ActionType::createIndex);
                actions.addAction(ActionType::dropCollection);
                actions.addAction(ActionType::renameCollectionSameDB);

                out->push_back(Privilege(ResourcePattern::forExactNamespace(outputNs), actions));
                out->push_back(Privilege(ResourcePattern::forExactNamespace(
                                                 NamespaceString(db, "system.indexes")),
                                         ActionType::find));
            }
        }
    }

    intrusive_ptr<Pipeline> Pipeline::splitForSharded() {
        // Create and initialize the shard spec we'll return. We start with an empty pipeline on the
        // shards and all work being done in the merger. Optimizations can move operations between
        // the pipelines to be more efficient.
        intrusive_ptr<Pipeline> shardPipeline(new Pipeline(pCtx));
        shardPipeline->explain = explain;

        // The order in which optimizations are applied can have significant impact on the
        // efficiency of the final pipeline. Be Careful!
        Optimizations::Sharded::findSplitPoint(shardPipeline.get(), this);

        return shardPipeline;
    }

    void Pipeline::Optimizations::Sharded::findSplitPoint(Pipeline* shardPipe,
                                                          Pipeline* mergePipe) {
        while (!mergePipe->sources.empty()) {
            intrusive_ptr<DocumentSource> current = mergePipe->sources.front();
            mergePipe->sources.pop_front();

            // Check if this source is splittable
            SplittableDocumentSource* splittable =
                dynamic_cast<SplittableDocumentSource*>(current.get());

            if (!splittable){
                // move the source from the merger sources to the shard sources
                shardPipe->sources.push_back(current);
            }
            else {
                // split this source into Merge and Shard sources
                intrusive_ptr<DocumentSource> shardSource = splittable->getShardSource();
                intrusive_ptr<DocumentSource> mergeSource = splittable->getMergeSource();
                if (shardSource) shardPipe->sources.push_back(shardSource);
                if (mergeSource) mergePipe->sources.push_front(mergeSource);

                break;
            }
        }
    }

    BSONObj Pipeline::getInitialQuery() const {
        if (sources.empty())
            return BSONObj();

        /* look for an initial $match */
        DocumentSourceMatch* match = dynamic_cast<DocumentSourceMatch*>(sources.front().get());
        if (!match)
            return BSONObj();

        return match->getQuery();
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
        serialized.setField(commandName, Value(pCtx->ns.coll()));
        serialized.setField(pipelineName, Value(array));

        if (explain) {
            serialized.setField(explainName, Value(explain));
        }

        if (pCtx->extSortAllowed) {
            serialized.setField("allowDiskUsage", Value(true));
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
        // should not get here in the explain case
        verify(!explain);

        // the array in which the aggregation results reside
        // cant use subArrayStart() due to error handling
        BSONArrayBuilder resultArray;
        DocumentSource* finalSource = sources.back().get();
        while (boost::optional<Document> next = finalSource->getNext()) {
            // add the document to the result set
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

    vector<Value> Pipeline::writeExplainOps() const {
        vector<Value> array;
        for(SourceContainer::const_iterator it = sources.begin(); it != sources.end(); ++it) {
            (*it)->serializeToArray(array, /*explain=*/true);
        }
        return array;
    }

    void Pipeline::addInitialSource(intrusive_ptr<DocumentSource> source) {
        sources.push_front(source);
    }

    bool Pipeline::canRunInMongos() const {
        if (pCtx->extSortAllowed)
            return false;

        if (explain)
            return false;

        if (dynamic_cast<DocumentSourceNeedsMongod*>(sources.back().get()))
            return false;

        return true;
    }

} // namespace mongo
