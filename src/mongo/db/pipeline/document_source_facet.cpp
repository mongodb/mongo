/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_facet.h"

#include <memory>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/tee_buffer.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::string;
using std::vector;

DocumentSourceFacet::DocumentSourceFacet(std::vector<FacetPipeline> facetPipelines,
                                         const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(expCtx),
      _teeBuffer(TeeBuffer::create(facetPipelines.size())),
      _facets(std::move(facetPipelines)) {
    for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
        auto& facet = _facets[facetId];
        facet.pipeline->addInitialSource(
            DocumentSourceTeeConsumer::create(pExpCtx, facetId, _teeBuffer));
    }
}

namespace {
/**
 * Extracts the names of the facets and the vectors of raw BSONObjs representing the stages within
 * that facet's pipeline.
 *
 * Throws a AssertionException if it fails to parse for any reason.
 */
vector<pair<string, vector<BSONObj>>> extractRawPipelines(const BSONElement& elem) {
    uassert(40169,
            str::stream() << "the $facet specification must be a non-empty object, but found: "
                          << elem,
            elem.type() == BSONType::Object && !elem.embeddedObject().isEmpty());

    vector<pair<string, vector<BSONObj>>> rawFacetPipelines;
    for (auto&& facetElem : elem.embeddedObject()) {
        const auto facetName = facetElem.fieldNameStringData();
        FieldPath::uassertValidFieldName(facetName);
        uassert(40170,
                str::stream() << "arguments to $facet must be arrays, " << facetName << " is type "
                              << typeName(facetElem.type()),
                facetElem.type() == BSONType::Array);

        vector<BSONObj> rawPipeline;
        for (auto&& subPipeElem : facetElem.Obj()) {
            uassert(40171,
                    str::stream() << "elements of arrays in $facet spec must be non-empty objects, "
                                  << facetName
                                  << " argument contained an element of type "
                                  << typeName(subPipeElem.type())
                                  << ": "
                                  << subPipeElem,
                    subPipeElem.type() == BSONType::Object);
            rawPipeline.push_back(subPipeElem.embeddedObject());
        }

        rawFacetPipelines.emplace_back(facetName.toString(), std::move(rawPipeline));
    }
    return rawFacetPipelines;
}
}  // namespace

std::unique_ptr<DocumentSourceFacet::LiteParsed> DocumentSourceFacet::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {
    std::vector<LiteParsedPipeline> liteParsedPipelines;

    for (auto&& rawPipeline : extractRawPipelines(spec)) {
        liteParsedPipelines.emplace_back(
            AggregationRequest(request.getNamespaceString(), rawPipeline.second));
    }

    PrivilegeVector requiredPrivileges;
    for (auto&& pipeline : liteParsedPipelines) {

        // A correct isMongos flag is only required for DocumentSourceCurrentOp which is disallowed
        // in $facet pipelines.
        const bool unusedIsMongosFlag = false;
        Privilege::addPrivilegesToPrivilegeVector(&requiredPrivileges,
                                                  pipeline.requiredPrivileges(unusedIsMongosFlag));
    }

    return stdx::make_unique<DocumentSourceFacet::LiteParsed>(std::move(liteParsedPipelines),
                                                              std::move(requiredPrivileges));
}

stdx::unordered_set<NamespaceString> DocumentSourceFacet::LiteParsed::getInvolvedNamespaces()
    const {
    stdx::unordered_set<NamespaceString> involvedNamespaces;
    for (auto&& liteParsedPipeline : _liteParsedPipelines) {
        auto involvedInSubPipe = liteParsedPipeline.getInvolvedNamespaces();
        involvedNamespaces.insert(involvedInSubPipe.begin(), involvedInSubPipe.end());
    }
    return involvedNamespaces;
}

REGISTER_DOCUMENT_SOURCE(facet,
                         DocumentSourceFacet::LiteParsed::parse,
                         DocumentSourceFacet::createFromBson);

intrusive_ptr<DocumentSourceFacet> DocumentSourceFacet::create(
    std::vector<FacetPipeline> facetPipelines, const intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceFacet(std::move(facetPipelines), expCtx);
}

void DocumentSourceFacet::setSource(DocumentSource* source) {
    _teeBuffer->setSource(source);
}

void DocumentSourceFacet::doDispose() {
    for (auto&& facet : _facets) {
        facet.pipeline.get_deleter().dismissDisposal();
        facet.pipeline->dispose(pExpCtx->opCtx);
    }
}

DocumentSource::GetNextResult DocumentSourceFacet::getNext() {
    pExpCtx->checkForInterrupt();

    if (_done) {
        return GetNextResult::makeEOF();
    }

    vector<vector<Value>> results(_facets.size());
    bool allPipelinesEOF = false;
    while (!allPipelinesEOF) {
        allPipelinesEOF = true;  // Set this to false if any pipeline isn't EOF.
        for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
            const auto& pipeline = _facets[facetId].pipeline;
            auto next = pipeline->getSources().back()->getNext();
            for (; next.isAdvanced(); next = pipeline->getSources().back()->getNext()) {
                results[facetId].emplace_back(next.releaseDocument());
            }
            allPipelinesEOF = allPipelinesEOF && next.isEOF();
        }
    }

    MutableDocument resultDoc;
    for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
        resultDoc[_facets[facetId].name] = Value(std::move(results[facetId]));
    }

    _done = true;  // We will only ever produce one result.
    return resultDoc.freeze();
}

Value DocumentSourceFacet::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument serialized;
    for (auto&& facet : _facets) {
        serialized[facet.name] = Value(explain ? facet.pipeline->writeExplainOps(*explain)
                                               : facet.pipeline->serialize());
    }
    return Value(Document{{"$facet", serialized.freezeToValue()}});
}

void DocumentSourceFacet::addInvolvedCollections(vector<NamespaceString>* collections) const {
    for (auto&& facet : _facets) {
        for (auto&& source : facet.pipeline->getSources()) {
            source->addInvolvedCollections(collections);
        }
    }
}

intrusive_ptr<DocumentSource> DocumentSourceFacet::optimize() {
    for (auto&& facet : _facets) {
        facet.pipeline->optimizePipeline();
    }
    return this;
}

void DocumentSourceFacet::detachFromOperationContext() {
    for (auto&& facet : _facets) {
        facet.pipeline->detachFromOperationContext();
    }
}

void DocumentSourceFacet::reattachToOperationContext(OperationContext* opCtx) {
    for (auto&& facet : _facets) {
        facet.pipeline->reattachToOperationContext(opCtx);
    }
}

DocumentSource::StageConstraints DocumentSourceFacet::constraints(
    Pipeline::SplitState pipeState) const {
    const bool mayUseDisk = std::any_of(_facets.begin(), _facets.end(), [&](const auto& facet) {
        const auto sources = facet.pipeline->getSources();
        return std::any_of(sources.begin(), sources.end(), [&](const auto source) {
            return source->constraints().diskRequirement == DiskUseRequirement::kWritesTmpData;
        });
    });

    // Currently we don't split $facet to have a merger part and a shards part (see SERVER-24154).
    // This means that if any stage in any of the $facet pipelines needs to run on the primary shard
    // or on mongoS, then the entire $facet stage must run there.
    static const std::set<HostTypeRequirement> definitiveHosts = {
        HostTypeRequirement::kMongoS, HostTypeRequirement::kPrimaryShard};

    HostTypeRequirement host = HostTypeRequirement::kNone;

    // Iterate through each pipeline to determine the HostTypeRequirement for the $facet stage.
    // Because we have already validated that there are no conflicting HostTypeRequirements during
    // parsing, if we observe any of the 'definitiveHosts' types in any of the pipelines then the
    // entire $facet stage must run on that host and iteration can stop. At the end of this process,
    // 'host' will be the $facet's final HostTypeRequirement.
    for (auto fi = _facets.begin(); fi != _facets.end() && !definitiveHosts.count(host); fi++) {
        const auto& sources = fi->pipeline->getSources();
        for (auto si = sources.begin(); si != sources.end() && !definitiveHosts.count(host); si++) {
            const auto hostReq = (*si)->constraints().resolvedHostTypeRequirement(pExpCtx);
            if (hostReq != HostTypeRequirement::kNone) {
                host = hostReq;
            }
        }
    }

    return {StreamType::kBlocking,
            PositionRequirement::kNone,
            host,
            mayUseDisk ? DiskUseRequirement::kWritesTmpData : DiskUseRequirement::kNoDiskUse,
            FacetRequirement::kNotAllowed,
            TransactionRequirement::kAllowed};
}

DepsTracker::State DocumentSourceFacet::getDependencies(DepsTracker* deps) const {
    const bool scopeHasVariables = pExpCtx->variablesParseState.hasDefinedVariables();
    for (auto&& facet : _facets) {
        auto subDepsTracker = facet.pipeline->getDependencies(deps->getMetadataAvailable());

        deps->fields.insert(subDepsTracker.fields.begin(), subDepsTracker.fields.end());
        deps->vars.insert(subDepsTracker.vars.begin(), subDepsTracker.vars.end());

        deps->needWholeDocument = deps->needWholeDocument || subDepsTracker.needWholeDocument;

        // The text score is the only type of metadata that could be needed by $facet.
        deps->setNeedsMetadata(
            DepsTracker::MetadataType::TEXT_SCORE,
            deps->getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE) ||
                subDepsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));

        // If there are variables defined at this stage's scope, there may be dependencies upon
        // them in subsequent pipelines. Keep enumerating.
        if (deps->needWholeDocument &&
            deps->getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE) && !scopeHasVariables) {
            break;
        }
    }

    // We will combine multiple documents into one, and the output document will have new fields, so
    // we will stop looking for dependencies at this point.
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

intrusive_ptr<DocumentSource> DocumentSourceFacet::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {

    boost::optional<std::string> needsMongoS;
    boost::optional<std::string> needsShard;

    std::vector<FacetPipeline> facetPipelines;
    for (auto&& rawFacet : extractRawPipelines(elem)) {
        const auto facetName = rawFacet.first;

        auto pipeline = uassertStatusOK(Pipeline::parseFacetPipeline(rawFacet.second, expCtx));

        // Validate that none of the facet pipelines have any conflicting HostTypeRequirements. This
        // verifies both that all stages within each pipeline are consistent, and that the pipelines
        // are consistent with one another.
        if (!needsShard && pipeline->needsShard()) {
            needsShard.emplace(facetName);
        }
        if (!needsMongoS && pipeline->needsMongosMerger()) {
            needsMongoS.emplace(facetName);
        }
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "$facet pipeline '" << *needsMongoS
                              << "' must run on mongoS, but '"
                              << *needsShard
                              << "' requires a shard",
                !(needsShard && needsMongoS));

        facetPipelines.emplace_back(facetName, std::move(pipeline));
    }

    return new DocumentSourceFacet(std::move(facetPipelines), expCtx);
}
}  // namespace mongo
