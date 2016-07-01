/**
 * Copyright 2011 (c) 10gen Inc.
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

#include "mongo/db/pipeline/document_source.h"

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/parsed_aggregation_projection.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;
using parsed_aggregation_projection::ParsedAggregationProjection;
using parsed_aggregation_projection::ProjectionType;

DocumentSourceProject::DocumentSourceProject(
    const intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<ParsedAggregationProjection> parsedProject)
    : DocumentSource(expCtx), _parsedProject(std::move(parsedProject)) {}

REGISTER_DOCUMENT_SOURCE(project, DocumentSourceProject::createFromBson);

const char* DocumentSourceProject::getSourceName() const {
    return "$project";
}

boost::optional<Document> DocumentSourceProject::getNext() {
    pExpCtx->checkForInterrupt();

    auto input = pSource->getNext();
    if (!input) {
        return boost::none;
    }

    return _parsedProject->applyProjection(*input);
}

intrusive_ptr<DocumentSource> DocumentSourceProject::optimize() {
    _parsedProject->optimize();
    return this;
}

Pipeline::SourceContainer::iterator DocumentSourceProject::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextSkip = dynamic_cast<DocumentSourceSkip*>((*std::next(itr)).get());
    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextSkip || nextLimit) {
        // Swap the $limit/$skip before ourselves, thus reducing the number of documents that
        // pass through the $project.
        std::swap(*itr, *std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    }
    return std::next(itr);
}

void DocumentSourceProject::dispose() {
    _parsedProject.reset();
}

Value DocumentSourceProject::serialize(bool explain) const {
    return Value(Document{{getSourceName(), _parsedProject->serialize(explain)}});
}

intrusive_ptr<DocumentSource> DocumentSourceProject::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(15969, "$project specification must be an object", elem.type() == Object);

    return new DocumentSourceProject(expCtx, ParsedAggregationProjection::create(elem.Obj()));
}

DocumentSource::GetDepsReturn DocumentSourceProject::getDependencies(DepsTracker* deps) const {
    // Add any fields referenced by the projection.
    _parsedProject->addDependencies(deps);

    if (_parsedProject->getType() == ProjectionType::kInclusion) {
        // Stop looking for further dependencies later in the pipeline, since anything that is not
        // explicitly included or added in this projection will not exist after this stage, so would
        // be pointless to include in our dependencies.
        return EXHAUSTIVE_FIELDS;
    } else {
        return SEE_NEXT;
    }
}

void DocumentSourceProject::doInjectExpressionContext() {
    _parsedProject->injectExpressionContext(pExpCtx);
}

}  // namespace mongo
