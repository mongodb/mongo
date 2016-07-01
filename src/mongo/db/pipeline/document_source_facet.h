/**
 *    Copyright (C) 2016 MongoDB, Inc.
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

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONElement;
class TeeBuffer;
class DocumentSourceTeeConsumer;
struct ExpressionContext;
class NamespaceString;
class Pipeline;

/**
 * A $facet stage contains multiple sub-pipelines. Each input to the $facet stage will feed into
 * each of the sub-pipelines. The $facet stage is blocking, and outputs only one document,
 * containing an array of results for each sub-pipeline.
 *
 * For example, {$facet: {facetA: [{$skip: 1}], facetB: [{$limit: 1}]}} would describe a $facet
 * stage which will produce a document like the following:
 * {facetA: [<all input documents except the first one>], facetB: [<the first document>]}.
 *
 * TODO SERVER-24154: Should inherit from SplittableDocumentSource so that it can split in a sharded
 * cluster.
 */
class DocumentSourceFacet final : public DocumentSourceNeedsMongod {
public:
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceFacet> create(
        StringMap<boost::intrusive_ptr<Pipeline>> facetPipelines,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Blocking call. Will consume all input and produces one output document.
     */
    boost::optional<Document> getNext() final;

    /**
     * Optimizes inner pipelines.
     */
    boost::intrusive_ptr<DocumentSource> optimize() final;

    /**
     * Injects the expression context into inner pipelines.
     */
    void doInjectExpressionContext() final;

    /**
     * Takes a union of all sub-pipelines, and adds them to 'deps'.
     */
    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    const char* getSourceName() const final {
        return "$facet";
    }

    /**
     * Sets 'source' as the source of '_teeBuffer'.
     */
    void setSource(DocumentSource* source) final;

    // The following are overridden just to forward calls to sub-pipelines.
    void addInvolvedCollections(std::vector<NamespaceString>* collections) const final;
    void doInjectMongodInterface(std::shared_ptr<MongodInterface> mongod) final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext(OperationContext* opCtx) final;

private:
    DocumentSourceFacet(StringMap<boost::intrusive_ptr<Pipeline>> facetPipelines,
                        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(bool explain = false) const final;

    boost::intrusive_ptr<TeeBuffer> _teeBuffer;
    StringMap<boost::intrusive_ptr<Pipeline>> _facetPipelines;

    bool _done = false;
};
}  // namespace mongo
