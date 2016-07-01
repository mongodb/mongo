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

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/tee_buffer.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

DocumentSourceFacet::DocumentSourceFacet(StringMap<intrusive_ptr<Pipeline>> facetPipelines,
                                         const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceNeedsMongod(expCtx), _facetPipelines(std::move(facetPipelines)) {

    // Build the tee stage, and the consumers of the tee.
    _teeBuffer = TeeBuffer::create();
    for (auto&& facet : _facetPipelines) {
        auto pipeline = facet.second;
        pipeline->addInitialSource(DocumentSourceTeeConsumer::create(pExpCtx, _teeBuffer));
    }
}

REGISTER_DOCUMENT_SOURCE(facet, DocumentSourceFacet::createFromBson);

intrusive_ptr<DocumentSourceFacet> DocumentSourceFacet::create(
    StringMap<intrusive_ptr<Pipeline>> facetPipelines,
    const intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceFacet(std::move(facetPipelines), expCtx);
}

void DocumentSourceFacet::setSource(DocumentSource* source) {
    _teeBuffer->setSource(source);
}

boost::optional<Document> DocumentSourceFacet::getNext() {
    pExpCtx->checkForInterrupt();

    if (_done) {
        return boost::none;
    }
    _done = true;  // We will only ever produce one result.

    // Build the results by executing each pipeline serially, one at a time.
    MutableDocument results;
    for (auto&& facet : _facetPipelines) {
        auto facetName = facet.first;
        auto facetPipeline = facet.second;

        std::vector<Value> facetResults;
        while (auto next = facetPipeline->getSources().back()->getNext()) {
            facetResults.emplace_back(std::move(*next));
        }
        results[facetName] = Value(std::move(facetResults));
    }

    _teeBuffer->dispose();  // Clear the buffer since we'll no longer need it.
    return results.freeze();
}

Value DocumentSourceFacet::serialize(bool explain) const {
    MutableDocument serialized;
    for (auto&& facet : _facetPipelines) {
        serialized[facet.first] =
            Value(explain ? facet.second->writeExplainOps() : facet.second->serialize());
    }
    return Value(Document{{"$facet", serialized.freezeToValue()}});
}

void DocumentSourceFacet::addInvolvedCollections(std::vector<NamespaceString>* collections) const {
    for (auto&& facet : _facetPipelines) {
        for (auto&& source : facet.second->getSources()) {
            source->addInvolvedCollections(collections);
        }
    }
}

intrusive_ptr<DocumentSource> DocumentSourceFacet::optimize() {
    for (auto&& facet : _facetPipelines) {
        facet.second->optimizePipeline();
    }
    return this;
}

void DocumentSourceFacet::doInjectExpressionContext() {
    for (auto&& facet : _facetPipelines) {
        facet.second->injectExpressionContext(pExpCtx);
    }
}

void DocumentSourceFacet::doInjectMongodInterface(std::shared_ptr<MongodInterface> mongod) {
    for (auto&& facet : _facetPipelines) {
        for (auto&& stage : facet.second->getSources()) {
            if (auto stageNeedingMongod = dynamic_cast<DocumentSourceNeedsMongod*>(stage.get())) {
                stageNeedingMongod->injectMongodInterface(mongod);
            }
        }
    }
}

void DocumentSourceFacet::doDetachFromOperationContext() {
    for (auto&& facet : _facetPipelines) {
        facet.second->detachFromOperationContext();
    }
}

void DocumentSourceFacet::doReattachToOperationContext(OperationContext* opCtx) {
    for (auto&& facet : _facetPipelines) {
        facet.second->reattachToOperationContext(opCtx);
    }
}

DocumentSource::GetDepsReturn DocumentSourceFacet::getDependencies(DepsTracker* deps) const {
    for (auto&& facet : _facetPipelines) {
        auto subDepsTracker = facet.second->getDependencies(deps->getMetadataAvailable());

        deps->fields.insert(subDepsTracker.fields.begin(), subDepsTracker.fields.end());

        deps->needWholeDocument = deps->needWholeDocument || subDepsTracker.needWholeDocument;
        deps->setNeedTextScore(deps->getNeedTextScore() || subDepsTracker.getNeedTextScore());

        if (deps->needWholeDocument && deps->getNeedTextScore()) {
            break;
        }
    }

    // We will combine multiple documents into one, and the output document will have new fields, so
    // we will stop looking for dependencies at this point.
    return GetDepsReturn::EXHAUSTIVE_ALL;
}

intrusive_ptr<DocumentSource> DocumentSourceFacet::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(40169,
            str::stream() << "the $facet specification must be a non-empty object, but found: "
                          << elem,
            elem.type() == BSONType::Object && !elem.embeddedObject().isEmpty());

    StringMap<intrusive_ptr<Pipeline>> facetPipelines;
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
                    str::stream() << "elements of arrays in $facet spec must be objects, "
                                  << facetName
                                  << " argument contained an element of type "
                                  << typeName(subPipeElem.type()),
                    subPipeElem.type() == BSONType::Object);

            rawPipeline.push_back(subPipeElem.embeddedObject());
        }

        auto pipeline = uassertStatusOK(Pipeline::parse(rawPipeline, expCtx));

        uassert(40172,
                str::stream() << "sub-pipelines in $facet stage cannot be empty: "
                              << facetElem.toString(),
                !pipeline->getSources().empty());

        // Disallow $out stages, $facet stages, and any stages that need to be the first stage in
        // the pipeline.
        for (auto&& stage : pipeline->getSources()) {
            if ((dynamic_cast<DocumentSourceOut*>(stage.get())) ||
                (dynamic_cast<DocumentSourceFacet*>(stage.get())) ||
                (stage->isValidInitialSource())) {
                uasserted(40173,
                          str::stream() << stage->getSourceName()
                                        << " is not allowed to be used within a $facet stage: "
                                        << facetElem.toString());
            }
        }

        facetPipelines[facetName] = pipeline;
    }

    return new DocumentSourceFacet(std::move(facetPipelines), expCtx);
}
}  // namespace mongo
