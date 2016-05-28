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

#include "mongo/platform/basic.h"

// This file defines functions from both of these headers
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_optimizations.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collation_serializer.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using boost::intrusive_ptr;
using std::endl;
using std::ostringstream;
using std::string;
using std::vector;

const char Pipeline::commandName[] = "aggregate";
const char Pipeline::pipelineName[] = "pipeline";
const char Pipeline::collationName[] = "collation";
const char Pipeline::explainName[] = "explain";
const char Pipeline::fromRouterName[] = "fromRouter";
const char Pipeline::serverPipelineName[] = "serverPipeline";
const char Pipeline::mongosPipelineName[] = "mongosPipeline";

Pipeline::Pipeline(const intrusive_ptr<ExpressionContext>& pTheCtx)
    : explain(false), pCtx(pTheCtx) {}

intrusive_ptr<Pipeline> Pipeline::parseCommand(string& errmsg,
                                               const BSONObj& cmdObj,
                                               const intrusive_ptr<ExpressionContext>& pCtx) {
    intrusive_ptr<Pipeline> pPipeline(new Pipeline(pCtx));
    vector<BSONElement> pipeline;
    repl::ReadConcernArgs readConcernArgs;

    /* gather the specification for the aggregation */
    for (BSONObj::iterator cmdIterator = cmdObj.begin(); cmdIterator.more();) {
        BSONElement cmdElement(cmdIterator.next());
        const char* pFieldName = cmdElement.fieldName();

        // ignore top-level fields prefixed with $. They are for the command processor, not us.
        if (pFieldName[0] == '$') {
            continue;
        }

        // maxTimeMS is also for the command processor.
        if (str::equals(pFieldName, LiteParsedQuery::cmdOptionMaxTimeMS)) {
            continue;
        }

        if (pFieldName == repl::ReadConcernArgs::kReadConcernFieldName) {
            uassertStatusOK(readConcernArgs.initialize(cmdElement));
            continue;
        }

        // ignore cursor options since they are handled externally.
        if (str::equals(pFieldName, "cursor")) {
            continue;
        }

        // ignore writeConcern since it's handled externally
        if (str::equals(pFieldName, "writeConcern")) {
            continue;
        }

        if (str::equals(pFieldName, collationName)) {
            uassert(ErrorCodes::BadValue,
                    str::stream() << collationName << " must be an object, not a "
                                  << typeName(cmdElement.type()),
                    cmdElement.type() == BSONType::Object);
            auto statusWithCollator =
                CollatorFactoryInterface::get(pCtx->opCtx->getServiceContext())
                    ->makeFromBSON(cmdElement.Obj());
            uassertStatusOK(statusWithCollator.getStatus());
            pCtx->collator = std::move(statusWithCollator.getValue());
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

        if (str::equals(pFieldName, "allowDiskUse")) {
            uassert(ErrorCodes::IllegalOperation,
                    "The 'allowDiskUse' option is not permitted in read-only mode.",
                    !storageGlobalParams.readOnly);

            uassert(16949,
                    str::stream() << "allowDiskUse must be a bool, not a "
                                  << typeName(cmdElement.type()),
                    cmdElement.type() == Bool);
            pCtx->extSortAllowed = cmdElement.Bool();
            continue;
        }

        if (pFieldName == bypassDocumentValidationCommandOption()) {
            pCtx->bypassDocumentValidation = cmdElement.trueValue();
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
    SourceContainer& sources = pPipeline->sources;  // shorthand

    /* iterate over the steps in the pipeline */
    const size_t nSteps = pipeline.size();
    for (size_t iStep = 0; iStep < nSteps; ++iStep) {
        /* pull out the pipeline element as an object */
        BSONElement pipeElement(pipeline[iStep]);
        uassert(15942,
                str::stream() << "pipeline element " << iStep << " is not an object",
                pipeElement.type() == Object);

        sources.push_back(DocumentSource::parse(pCtx, pipeElement.Obj()));
        if (sources.back()->isValidInitialSource()) {
            uassert(28837,
                    str::stream() << sources.back()->getSourceName()
                                  << " is only valid as the first stage in a pipeline.",
                    iStep == 0);
        }

        if (dynamic_cast<DocumentSourceOut*>(sources.back().get())) {
            uassert(16991, "$out can only be the final stage in the pipeline", iStep == nSteps - 1);

            uassert(ErrorCodes::InvalidOptions,
                    "$out can only be used with the 'local' read concern level",
                    readConcernArgs.getLevel() == repl::ReadConcernLevel::kLocalReadConcern);
        }
    }

    pPipeline->optimizePipeline();

    return pPipeline;
}

void Pipeline::optimizePipeline() {
    SourceContainer optimizedSources;

    SourceContainer::iterator itr = sources.begin();

    while (itr != sources.end() && std::next(itr) != sources.end()) {
        invariant((*itr).get());
        itr = (*itr).get()->optimizeAt(itr, &sources);
    }

    // Once we have reached our final number of stages, optimize each individually.
    for (auto&& source : sources) {
        if (auto out = source->optimize()) {
            optimizedSources.push_back(out);
        }
    }
    sources.swap(optimizedSources);
}

Status Pipeline::checkAuthForCommand(ClientBasic* client,
                                     const std::string& db,
                                     const BSONObj& cmdObj) {
    NamespaceString inputNs(db, cmdObj.firstElement().str());
    auto inputResource = ResourcePattern::forExactNamespace(inputNs);
    uassert(17138,
            mongoutils::str::stream() << "Invalid input namespace, " << inputNs.ns(),
            inputNs.isValid());

    std::vector<Privilege> privileges;

    if (cmdObj.getFieldDotted("pipeline.0.$indexStats")) {
        Privilege::addPrivilegeToPrivilegeVector(
            &privileges,
            Privilege(ResourcePattern::forAnyNormalResource(), ActionType::indexStats));
    } else {
        // If no source requiring an alternative permission scheme is specified then default to
        // requiring find() privileges on the given namespace.
        Privilege::addPrivilegeToPrivilegeVector(&privileges,
                                                 Privilege(inputResource, ActionType::find));
    }

    BSONObj pipeline = cmdObj.getObjectField("pipeline");
    BSONForEach(stageElem, pipeline) {
        BSONObj stage = stageElem.embeddedObjectUserCheck();
        StringData stageName = stage.firstElementFieldName();
        if (stageName == "$out" && stage.firstElementType() == String) {
            NamespaceString outputNs(db, stage.firstElement().str());
            uassert(17139,
                    mongoutils::str::stream() << "Invalid $out target namespace, " << outputNs.ns(),
                    outputNs.isValid());

            ActionSet actions;
            actions.addAction(ActionType::remove);
            actions.addAction(ActionType::insert);
            if (shouldBypassDocumentValidationForCommand(cmdObj)) {
                actions.addAction(ActionType::bypassDocumentValidation);
            }
            Privilege::addPrivilegeToPrivilegeVector(
                &privileges, Privilege(ResourcePattern::forExactNamespace(outputNs), actions));
        } else if (stageName == "$lookup" && stage.firstElementType() == Object) {
            NamespaceString fromNs(db, stage.firstElement()["from"].str());
            Privilege::addPrivilegeToPrivilegeVector(
                &privileges,
                Privilege(ResourcePattern::forExactNamespace(fromNs), ActionType::find));
        }
    }

    if (AuthorizationSession::get(client)->isAuthorizedForPrivileges(privileges))
        return Status::OK();
    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

bool Pipeline::aggSupportsWriteConcern(const BSONObj& cmd) {
    if (cmd.hasField("pipeline") == false) {
        return false;
    }

    auto stages = cmd["pipeline"].Array();
    for (auto stage : stages) {
        if (stage.Obj().hasField("$out")) {
            return true;
        }
    }

    return false;
}

void Pipeline::detachFromOperationContext() {
    pCtx->opCtx = nullptr;

    for (auto* source : sourcesNeedingMongod) {
        source->setOperationContext(nullptr);
    }
}

void Pipeline::reattachToOperationContext(OperationContext* opCtx) {
    invariant(pCtx->opCtx == nullptr);
    pCtx->opCtx = opCtx;

    for (auto* source : sourcesNeedingMongod) {
        source->setOperationContext(opCtx);
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
    Optimizations::Sharded::moveFinalUnwindFromShardsToMerger(shardPipeline.get(), this);
    Optimizations::Sharded::limitFieldsSentFromShardsToMerger(shardPipeline.get(), this);

    return shardPipeline;
}

void Pipeline::Optimizations::Sharded::findSplitPoint(Pipeline* shardPipe, Pipeline* mergePipe) {
    while (!mergePipe->sources.empty()) {
        intrusive_ptr<DocumentSource> current = mergePipe->sources.front();
        mergePipe->sources.pop_front();

        // Check if this source is splittable
        SplittableDocumentSource* splittable =
            dynamic_cast<SplittableDocumentSource*>(current.get());

        if (!splittable) {
            // move the source from the merger sources to the shard sources
            shardPipe->sources.push_back(current);
        } else {
            // split this source into Merge and Shard sources
            intrusive_ptr<DocumentSource> shardSource = splittable->getShardSource();
            intrusive_ptr<DocumentSource> mergeSource = splittable->getMergeSource();
            if (shardSource)
                shardPipe->sources.push_back(shardSource);
            if (mergeSource)
                mergePipe->sources.push_front(mergeSource);

            break;
        }
    }
}

void Pipeline::Optimizations::Sharded::moveFinalUnwindFromShardsToMerger(Pipeline* shardPipe,
                                                                         Pipeline* mergePipe) {
    while (!shardPipe->sources.empty() &&
           dynamic_cast<DocumentSourceUnwind*>(shardPipe->sources.back().get())) {
        mergePipe->sources.push_front(shardPipe->sources.back());
        shardPipe->sources.pop_back();
    }
}

void Pipeline::Optimizations::Sharded::limitFieldsSentFromShardsToMerger(Pipeline* shardPipe,
                                                                         Pipeline* mergePipe) {
    DepsTracker mergeDeps = mergePipe->getDependencies(shardPipe->getInitialQuery());
    if (mergeDeps.needWholeDocument)
        return;  // the merge needs all fields, so nothing we can do.

    // Empty project is "special" so if no fields are needed, we just ask for _id instead.
    if (mergeDeps.fields.empty())
        mergeDeps.fields.insert("_id");

    // Remove metadata from dependencies since it automatically flows through projection and we
    // don't want to project it in to the document.
    mergeDeps.needTextScore = false;

    // HEURISTIC: only apply optimization if none of the shard stages have an exhaustive list of
    // field dependencies. While this may not be 100% ideal in all cases, it is simple and
    // avoids the worst cases by ensuring that:
    // 1) Optimization IS applied when the shards wouldn't have known their exhaustive list of
    //    dependencies. This situation can happen when a $sort is before the first $project or
    //    $group. Without the optimization, the shards would have to reify and transmit full
    //    objects even though only a subset of fields are needed.
    // 2) Optimization IS NOT applied immediately following a $project or $group since it would
    //    add an unnecessary project (and therefore a deep-copy).
    for (auto&& source : shardPipe->sources) {
        DepsTracker dt;
        if (source->getDependencies(&dt) & DocumentSource::EXHAUSTIVE_FIELDS)
            return;
    }
    // if we get here, add the project.
    shardPipe->sources.push_back(DocumentSourceProject::createFromBson(
        BSON("$project" << mergeDeps.toProjection()).firstElement(), shardPipe->pCtx));
}

BSONObj Pipeline::getInitialQuery() const {
    if (sources.empty())
        return BSONObj();

    /* look for an initial $match */
    DocumentSourceMatch* match = dynamic_cast<DocumentSourceMatch*>(sources.front().get());
    if (match) {
        return match->getQuery();
    }

    DocumentSourceGeoNear* geoNear = dynamic_cast<DocumentSourceGeoNear*>(sources.front().get());
    if (geoNear) {
        return geoNear->getQuery();
    }

    return BSONObj();
}

bool Pipeline::needsPrimaryShardMerger() const {
    for (auto&& source : sources) {
        if (source->needsPrimaryShard()) {
            return true;
        }
    }
    return false;
}

std::vector<NamespaceString> Pipeline::getInvolvedCollections() const {
    std::vector<NamespaceString> collections;
    for (auto&& source : sources) {
        source->addInvolvedCollections(&collections);
    }
    return collections;
}

Document Pipeline::serialize() const {
    MutableDocument serialized;
    // create an array out of the pipeline operations
    vector<Value> array;
    for (SourceContainer::const_iterator iter(sources.begin()), listEnd(sources.end());
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
        serialized.setField("allowDiskUse", Value(true));
    }

    if (pCtx->bypassDocumentValidation) {
        serialized.setField(bypassDocumentValidationCommandOption(), Value(true));
    }

    if (pCtx->collator.get()) {
        serialized.setField(collationName,
                            Value(CollationSerializer::specToBSON(pCtx->collator->getSpec())));
    }

    return serialized.freeze();
}

void Pipeline::stitch() {
    massert(16600, "should not have an empty pipeline", !sources.empty());

    /* chain together the sources we found */
    DocumentSource* prevSource = sources.front().get();
    for (SourceContainer::iterator iter(++sources.begin()), listEnd(sources.end()); iter != listEnd;
         ++iter) {
        intrusive_ptr<DocumentSource> pTemp(*iter);
        pTemp->setSource(prevSource);
        prevSource = pTemp.get();
    }

    // Cache the document sources that have a MongodInterface to avoid the cost of a dynamic cast
    // when updating the OperationContext of their DBDirectClients while executing the pipeline.
    for (auto&& source : sources) {
        if (auto* needsMongod = dynamic_cast<DocumentSourceNeedsMongod*>(source.get())) {
            sourcesNeedingMongod.push_back(needsMongod);
        }
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
        BSONObjBuilder documentBuilder(resultArray.subobjStart());
        next->toBson(&documentBuilder);
        documentBuilder.doneFast();
        // object will be too large, assert. the extra 1KB is for headers
        uassert(16389,
                str::stream() << "aggregation result exceeds maximum document size ("
                              << BSONObjMaxUserSize / (1024 * 1024)
                              << "MB)",
                resultArray.len() < BSONObjMaxUserSize - 1024);
    }

    resultArray.done();
    result.appendArray("result", resultArray.arr());
}

vector<Value> Pipeline::writeExplainOps() const {
    vector<Value> array;
    for (SourceContainer::const_iterator it = sources.begin(); it != sources.end(); ++it) {
        (*it)->serializeToArray(array, /*explain=*/true);
    }
    return array;
}

void Pipeline::addInitialSource(intrusive_ptr<DocumentSource> source) {
    sources.push_front(source);
}

DepsTracker Pipeline::getDependencies(const BSONObj& initialQuery) const {
    DepsTracker deps;
    bool knowAllFields = false;
    bool knowAllMeta = false;
    for (auto&& source : sources) {
        DepsTracker localDeps;
        DocumentSource::GetDepsReturn status = source->getDependencies(&localDeps);

        if (status == DocumentSource::NOT_SUPPORTED) {
            // Assume this stage needs everything. We may still know something about our
            // dependencies if an earlier stage returned either EXHAUSTIVE_FIELDS or
            // EXHAUSTIVE_META.
            break;
        }

        if (!knowAllFields) {
            deps.fields.insert(localDeps.fields.begin(), localDeps.fields.end());
            if (localDeps.needWholeDocument)
                deps.needWholeDocument = true;
            knowAllFields = status & DocumentSource::EXHAUSTIVE_FIELDS;
        }

        if (!knowAllMeta) {
            if (localDeps.needTextScore)
                deps.needTextScore = true;

            knowAllMeta = status & DocumentSource::EXHAUSTIVE_META;
        }

        if (knowAllMeta && knowAllFields) {
            break;
        }
    }

    if (!knowAllFields)
        deps.needWholeDocument = true;  // don't know all fields we need

    // NOTE This code assumes that textScore can only be generated by the initial query.
    if (DocumentSourceMatch::isTextQuery(initialQuery)) {
        // If doing a text query, assume we need the score if we can't prove we don't.
        if (!knowAllMeta)
            deps.needTextScore = true;
    } else {
        // If we aren't doing a text query, then we don't need to ask for the textScore since we
        // know it will be missing anyway.
        deps.needTextScore = false;
    }

    return deps;
}
}  // namespace mongo
