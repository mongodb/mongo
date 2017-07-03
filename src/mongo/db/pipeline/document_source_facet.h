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
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {

class BSONElement;
class TeeBuffer;
class DocumentSourceTeeConsumer;
class ExpressionContext;
class NamespaceString;

/**
 * A $facet stage contains multiple sub-pipelines. Each input to the $facet stage will feed into
 * each of the sub-pipelines. The $facet stage is blocking, and outputs only one document,
 * containing an array of results for each sub-pipeline.
 *
 * For example, {$facet: {facetA: [{$skip: 1}], facetB: [{$limit: 1}]}} would describe a $facet
 * stage which will produce a document like the following:
 * {facetA: [<all input documents except the first one>], facetB: [<the first document>]}.
 */
class DocumentSourceFacet final : public DocumentSourceNeedsMongod,
                                  public SplittableDocumentSource {
public:
    struct FacetPipeline {
        FacetPipeline(std::string name, std::unique_ptr<Pipeline, Pipeline::Deleter> pipeline)
            : name(std::move(name)), pipeline(std::move(pipeline)) {}

        std::string name;
        std::unique_ptr<Pipeline, Pipeline::Deleter> pipeline;
    };

    class LiteParsed : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec);

        LiteParsed(std::vector<LiteParsedPipeline> liteParsedPipelines, PrivilegeVector privileges)
            : _liteParsedPipelines(std::move(liteParsedPipelines)),
              _requiredPrivileges(std::move(privileges)) {}

        PrivilegeVector requiredPrivileges(bool isMongos) const final {
            return _requiredPrivileges;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final;

    private:
        const std::vector<LiteParsedPipeline> _liteParsedPipelines;
        const PrivilegeVector _requiredPrivileges;
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceFacet> create(
        std::vector<FacetPipeline> facetPipelines,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Blocking call. Will consume all input and produces one output document.
     */
    GetNextResult getNext() final;

    /**
     * Optimizes inner pipelines.
     */
    boost::intrusive_ptr<DocumentSource> optimize() final;

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

    /**
     * The $facet stage must be run on the merging shard.
     *
     * TODO SERVER-24154: Should be smarter about splitting so that parts of the sub-pipelines can
     * potentially be run in parallel on multiple shards.
     */
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }
    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    // The following are overridden just to forward calls to sub-pipelines.
    void addInvolvedCollections(std::vector<NamespaceString>* collections) const final;
    void doInjectMongodInterface(std::shared_ptr<MongodInterface> mongod) final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext(OperationContext* opCtx) final;
    bool needsPrimaryShard() const final;

protected:
    void doDispose() final;

private:
    DocumentSourceFacet(std::vector<FacetPipeline> facetPipelines,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    boost::intrusive_ptr<TeeBuffer> _teeBuffer;
    std::vector<FacetPipeline> _facets;

    bool _done = false;
};
}  // namespace mongo
