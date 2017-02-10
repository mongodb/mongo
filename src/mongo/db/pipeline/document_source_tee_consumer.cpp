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

#include "mongo/db/pipeline/document_source_tee_consumer.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceTeeConsumer::DocumentSourceTeeConsumer(const intrusive_ptr<ExpressionContext>& expCtx,
                                                     size_t facetId,
                                                     const intrusive_ptr<TeeBuffer>& bufferSource)
    : DocumentSource(expCtx), _facetId(facetId), _bufferSource(bufferSource) {}

boost::intrusive_ptr<DocumentSourceTeeConsumer> DocumentSourceTeeConsumer::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    size_t facetId,
    const boost::intrusive_ptr<TeeBuffer>& bufferSource) {
    return new DocumentSourceTeeConsumer(expCtx, facetId, bufferSource);
}

DocumentSource::GetNextResult DocumentSourceTeeConsumer::getNext() {
    pExpCtx->checkForInterrupt();
    return _bufferSource->getNext(_facetId);
}

void DocumentSourceTeeConsumer::dispose() {
    _bufferSource->dispose(_facetId);
}

Value DocumentSourceTeeConsumer::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    // This stage will be inserted into the beginning of a pipeline, but should not show up in the
    // explain output.
    return Value();
}
}  // namespace mongo
