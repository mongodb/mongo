/**
 *    Copyright (C) 2015 MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source.h"

#include <boost/math/distributions/beta.hpp>

#include "mongo/db/client.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/log.h"

namespace mongo {
using boost::intrusive_ptr;

DocumentSourceSampleFromRandomCursor::DocumentSourceSampleFromRandomCursor(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    long long size,
    std::string idField,
    long long nDocsInCollection)
    : DocumentSource(pExpCtx),
      _size(size),
      _idField(std::move(idField)),
      _nDocsInColl(nDocsInCollection) {}

const char* DocumentSourceSampleFromRandomCursor::getSourceName() const {
    return "$sampleFromRandomCursor";
}

namespace {
/**
 * Select a random value drawn according to the distribution Beta(alpha=1, beta=N). The kth smallest
 * value of a sample of size N from a Uniform(0, 1) distribution has a Beta(k, N + 1 - k)
 * distribution, so the return value represents the smallest value from such a sample. This is also
 * the expected distance between the values drawn from a uniform distribution, which is how it is
 * being used here.
 */
double smallestFromSampleOfUniform(PseudoRandom* prng, size_t N) {
    boost::math::beta_distribution<double> betaDist(1.0, static_cast<double>(N));
    double p = prng->nextCanonicalDouble();
    return boost::math::quantile(betaDist, p);
}
}  // namespace

boost::optional<Document> DocumentSourceSampleFromRandomCursor::getNext() {
    pExpCtx->checkForInterrupt();

    if (_seenDocs->size() >= static_cast<size_t>(_size))
        return {};

    auto doc = getNextNonDuplicateDocument();
    if (!doc)
        return {};

    // Assign it a random value to enable merging by random value, attempting to avoid bias in that
    // process.
    auto& prng = pExpCtx->opCtx->getClient()->getPrng();
    _randMetaFieldVal -= smallestFromSampleOfUniform(&prng, _nDocsInColl);

    MutableDocument md(std::move(*doc));
    md.setRandMetaField(_randMetaFieldVal);
    return md.freeze();
}

boost::optional<Document> DocumentSourceSampleFromRandomCursor::getNextNonDuplicateDocument() {
    // We may get duplicate documents back from the random cursor, and should not return duplicate
    // documents, so keep trying until we get a new one.
    const int kMaxAttempts = 100;
    for (int i = 0; i < kMaxAttempts; ++i) {
        auto doc = pSource->getNext();
        if (!doc)
            return doc;

        auto idField = (*doc)[_idField];
        uassert(
            28793,
            str::stream()
                << "The optimized $sample stage requires all documents have a "
                << _idField
                << " field in order to de-duplicate results, but encountered a document without a "
                << _idField
                << " field: "
                << (*doc).toString(),
            !idField.missing());

        if (_seenDocs->insert(std::move(idField)).second) {
            return doc;
        }
        LOG(1) << "$sample encountered duplicate document: " << (*doc).toString() << std::endl;
    }
    uasserted(28799,
              str::stream() << "$sample stage could not find a non-duplicate document after "
                            << kMaxAttempts
                            << " while using a random cursor. This is likely a "
                               "sporadic failure, please try again.");
}

Value DocumentSourceSampleFromRandomCursor::serialize(bool explain) const {
    return Value(DOC(getSourceName() << DOC("size" << _size)));
}

DocumentSource::GetDepsReturn DocumentSourceSampleFromRandomCursor::getDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(_idField);
    return SEE_NEXT;
}

void DocumentSourceSampleFromRandomCursor::doInjectExpressionContext() {
    _seenDocs = pExpCtx->getValueComparator().makeUnorderedValueSet();
}

intrusive_ptr<DocumentSourceSampleFromRandomCursor> DocumentSourceSampleFromRandomCursor::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    long long size,
    std::string idField,
    long long nDocsInCollection) {
    intrusive_ptr<DocumentSourceSampleFromRandomCursor> source(
        new DocumentSourceSampleFromRandomCursor(expCtx, size, idField, nDocsInCollection));
    source->injectExpressionContext(expCtx);
    return source;
}
}  // mongo
