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

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using boost::intrusive_ptr;
using std::endl;
using std::ostringstream;
using std::string;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

Pipeline::Pipeline(const intrusive_ptr<ExpressionContext>& pTheCtx) : pCtx(pTheCtx) {}

Pipeline::Pipeline(SourceContainer stages, const intrusive_ptr<ExpressionContext>& expCtx)
    : _sources(stages), pCtx(expCtx) {}

StatusWith<intrusive_ptr<Pipeline>> Pipeline::parse(
    const std::vector<BSONObj>& rawPipeline, const intrusive_ptr<ExpressionContext>& expCtx) {
    intrusive_ptr<Pipeline> pipeline(new Pipeline(expCtx));

    for (auto&& stageObj : rawPipeline) {
        auto parsedSources = DocumentSource::parse(expCtx, stageObj);
        pipeline->_sources.insert(
            pipeline->_sources.end(), parsedSources.begin(), parsedSources.end());
    }

    auto status = pipeline->ensureAllStagesAreInLegalPositions();
    if (!status.isOK()) {
        return status;
    }
    pipeline->stitch();
    return pipeline;
}

StatusWith<intrusive_ptr<Pipeline>> Pipeline::create(
    SourceContainer stages, const intrusive_ptr<ExpressionContext>& expCtx) {
    intrusive_ptr<Pipeline> pipeline(new Pipeline(stages, expCtx));
    auto status = pipeline->ensureAllStagesAreInLegalPositions();
    if (!status.isOK()) {
        return status;
    }
    pipeline->stitch();
    return pipeline;
}

Status Pipeline::ensureAllStagesAreInLegalPositions() const {
    size_t i = 0;
    for (auto&& stage : _sources) {
        if (stage->isValidInitialSource() && i != 0) {
            return {ErrorCodes::BadValue,
                    str::stream() << stage->getSourceName()
                                  << " is only valid as the first stage in a pipeline."};
        }

        if (dynamic_cast<DocumentSourceOut*>(stage.get()) && i != _sources.size() - 1) {
            return {ErrorCodes::BadValue, "$out can only be the final stage in the pipeline"};
        }
        ++i;
    }
    return Status::OK();
}

void Pipeline::optimizePipeline() {
    SourceContainer optimizedSources;

    SourceContainer::iterator itr = _sources.begin();

    while (itr != _sources.end() && std::next(itr) != _sources.end()) {
        invariant((*itr).get());
        itr = (*itr).get()->optimizeAt(itr, &_sources);
    }

    // Once we have reached our final number of stages, optimize each individually.
    for (auto&& source : _sources) {
        if (auto out = source->optimize()) {
            optimizedSources.push_back(out);
        }
    }
    _sources.swap(optimizedSources);
    stitch();
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

    if (dps::extractElementAtPath(cmdObj, "pipeline.0.$indexStats")) {
        Privilege::addPrivilegeToPrivilegeVector(
            &privileges,
            Privilege(ResourcePattern::forAnyNormalResource(), ActionType::indexStats));
    } else if (dps::extractElementAtPath(cmdObj, "pipeline.0.$collStats")) {
        Privilege::addPrivilegeToPrivilegeVector(&privileges,
                                                 Privilege(inputResource, ActionType::collStats));
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
        } else if (stageName == "$graphLookup" && stage.firstElementType() == Object) {
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

    for (auto&& source : _sources) {
        source->detachFromOperationContext();
    }
}

void Pipeline::reattachToOperationContext(OperationContext* opCtx) {
    invariant(pCtx->opCtx == nullptr);
    pCtx->opCtx = opCtx;

    for (auto&& source : _sources) {
        source->reattachToOperationContext(opCtx);
    }
}

void Pipeline::injectExpressionContext(const intrusive_ptr<ExpressionContext>& expCtx) {
    pCtx = expCtx;
    for (auto&& stage : _sources) {
        stage->injectExpressionContext(pCtx);
    }
}

intrusive_ptr<Pipeline> Pipeline::splitForSharded() {
    // Create and initialize the shard spec we'll return. We start with an empty pipeline on the
    // shards and all work being done in the merger. Optimizations can move operations between
    // the pipelines to be more efficient.
    intrusive_ptr<Pipeline> shardPipeline(new Pipeline(pCtx));

    // The order in which optimizations are applied can have significant impact on the
    // efficiency of the final pipeline. Be Careful!
    Optimizations::Sharded::findSplitPoint(shardPipeline.get(), this);
    Optimizations::Sharded::moveFinalUnwindFromShardsToMerger(shardPipeline.get(), this);
    Optimizations::Sharded::limitFieldsSentFromShardsToMerger(shardPipeline.get(), this);

    return shardPipeline;
}

void Pipeline::Optimizations::Sharded::findSplitPoint(Pipeline* shardPipe, Pipeline* mergePipe) {
    while (!mergePipe->_sources.empty()) {
        intrusive_ptr<DocumentSource> current = mergePipe->_sources.front();
        mergePipe->_sources.pop_front();

        // Check if this source is splittable
        SplittableDocumentSource* splittable =
            dynamic_cast<SplittableDocumentSource*>(current.get());

        if (!splittable) {
            // move the source from the merger _sources to the shard _sources
            shardPipe->_sources.push_back(current);
        } else {
            // split this source into Merge and Shard _sources
            intrusive_ptr<DocumentSource> shardSource = splittable->getShardSource();
            intrusive_ptr<DocumentSource> mergeSource = splittable->getMergeSource();
            if (shardSource)
                shardPipe->_sources.push_back(shardSource);
            if (mergeSource)
                mergePipe->_sources.push_front(mergeSource);

            break;
        }
    }
}

void Pipeline::Optimizations::Sharded::moveFinalUnwindFromShardsToMerger(Pipeline* shardPipe,
                                                                         Pipeline* mergePipe) {
    while (!shardPipe->_sources.empty() &&
           dynamic_cast<DocumentSourceUnwind*>(shardPipe->_sources.back().get())) {
        mergePipe->_sources.push_front(shardPipe->_sources.back());
        shardPipe->_sources.pop_back();
    }
}

void Pipeline::Optimizations::Sharded::limitFieldsSentFromShardsToMerger(Pipeline* shardPipe,
                                                                         Pipeline* mergePipe) {
    DepsTracker mergeDeps(
        mergePipe->getDependencies(DocumentSourceMatch::isTextQuery(shardPipe->getInitialQuery())
                                       ? DepsTracker::MetadataAvailable::kTextScore
                                       : DepsTracker::MetadataAvailable::kNoMetadata));
    if (mergeDeps.needWholeDocument)
        return;  // the merge needs all fields, so nothing we can do.

    // Empty project is "special" so if no fields are needed, we just ask for _id instead.
    if (mergeDeps.fields.empty())
        mergeDeps.fields.insert("_id");

    // Remove metadata from dependencies since it automatically flows through projection and we
    // don't want to project it in to the document.
    mergeDeps.setNeedTextScore(false);

    // HEURISTIC: only apply optimization if none of the shard stages have an exhaustive list of
    // field dependencies. While this may not be 100% ideal in all cases, it is simple and
    // avoids the worst cases by ensuring that:
    // 1) Optimization IS applied when the shards wouldn't have known their exhaustive list of
    //    dependencies. This situation can happen when a $sort is before the first $project or
    //    $group. Without the optimization, the shards would have to reify and transmit full
    //    objects even though only a subset of fields are needed.
    // 2) Optimization IS NOT applied immediately following a $project or $group since it would
    //    add an unnecessary project (and therefore a deep-copy).
    for (auto&& source : shardPipe->_sources) {
        DepsTracker dt;
        if (source->getDependencies(&dt) & DocumentSource::EXHAUSTIVE_FIELDS)
            return;
    }
    // if we get here, add the project.
    shardPipe->_sources.push_back(DocumentSourceProject::createFromBson(
        BSON("$project" << mergeDeps.toProjection()).firstElement(), shardPipe->pCtx));
}

BSONObj Pipeline::getInitialQuery() const {
    if (_sources.empty())
        return BSONObj();

    /* look for an initial $match */
    DocumentSourceMatch* match = dynamic_cast<DocumentSourceMatch*>(_sources.front().get());
    if (match) {
        return match->getQuery();
    }

    DocumentSourceGeoNear* geoNear = dynamic_cast<DocumentSourceGeoNear*>(_sources.front().get());
    if (geoNear) {
        return geoNear->getQuery();
    }

    return BSONObj();
}

bool Pipeline::needsPrimaryShardMerger() const {
    for (auto&& source : _sources) {
        if (source->needsPrimaryShard()) {
            return true;
        }
    }
    return false;
}

std::vector<NamespaceString> Pipeline::getInvolvedCollections() const {
    std::vector<NamespaceString> collections;
    for (auto&& source : _sources) {
        source->addInvolvedCollections(&collections);
    }
    return collections;
}

vector<Value> Pipeline::serialize() const {
    vector<Value> serializedSources;
    for (auto&& source : _sources) {
        source->serializeToArray(serializedSources);
    }
    return serializedSources;
}

void Pipeline::stitch() {
    if (_sources.empty()) {
        return;
    }
    // Chain together all the stages.
    DocumentSource* prevSource = _sources.front().get();
    for (SourceContainer::iterator iter(++_sources.begin()), listEnd(_sources.end());
         iter != listEnd;
         ++iter) {
        intrusive_ptr<DocumentSource> pTemp(*iter);
        pTemp->setSource(prevSource);
        prevSource = pTemp.get();
    }
}

void Pipeline::run(BSONObjBuilder& result) {
    // We should not get here in the explain case.
    verify(!pCtx->isExplain);

    // the array in which the aggregation results reside
    // cant use subArrayStart() due to error handling
    BSONArrayBuilder resultArray;
    DocumentSource* finalSource = _sources.back().get();
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
    for (SourceContainer::const_iterator it = _sources.begin(); it != _sources.end(); ++it) {
        (*it)->serializeToArray(array, /*explain=*/true);
    }
    return array;
}

void Pipeline::addInitialSource(intrusive_ptr<DocumentSource> source) {
    if (!_sources.empty()) {
        _sources.front()->setSource(source.get());
    }
    _sources.push_front(source);
}

DepsTracker Pipeline::getDependencies(DepsTracker::MetadataAvailable metadataAvailable) const {
    DepsTracker deps(metadataAvailable);
    bool knowAllFields = false;
    bool knowAllMeta = false;
    for (auto&& source : _sources) {
        DepsTracker localDeps(deps.getMetadataAvailable());
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
            if (localDeps.getNeedTextScore())
                deps.setNeedTextScore(true);

            knowAllMeta = status & DocumentSource::EXHAUSTIVE_META;
        }

        if (knowAllMeta && knowAllFields) {
            break;
        }
    }

    if (!knowAllFields)
        deps.needWholeDocument = true;  // don't know all fields we need

    if (metadataAvailable & DepsTracker::MetadataAvailable::kTextScore) {
        // If there is a text score, assume we need to keep it if we can't prove we don't. If we are
        // the first half of a pipeline which has been split, future stages might need it.
        if (!knowAllMeta)
            deps.setNeedTextScore(true);
    } else {
        // If there is no text score available, then we don't need to ask for it.
        deps.setNeedTextScore(false);
    }

    return deps;
}

}  // namespace mongo
