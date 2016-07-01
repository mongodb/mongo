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

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/client.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
using boost::intrusive_ptr;

DocumentSourceSample::DocumentSourceSample(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), _size(0) {}

REGISTER_DOCUMENT_SOURCE(sample, DocumentSourceSample::createFromBson);

const char* DocumentSourceSample::getSourceName() const {
    return "$sample";
}

boost::optional<Document> DocumentSourceSample::getNext() {
    if (_size == 0)
        return {};

    pExpCtx->checkForInterrupt();

    if (!_sortStage->isPopulated()) {
        // Exhaust source stage, add random metadata, and push all into sorter.
        PseudoRandom& prng = pExpCtx->opCtx->getClient()->getPrng();
        while (boost::optional<Document> next = pSource->getNext()) {
            MutableDocument doc(std::move(*next));
            doc.setRandMetaField(prng.nextCanonicalDouble());
            _sortStage->loadDocument(doc.freeze());
        }
        _sortStage->loadingDone();
    }

    invariant(_sortStage->isPopulated());
    return _sortStage->getNext();
}

Value DocumentSourceSample::serialize(bool explain) const {
    return Value(DOC(getSourceName() << DOC("size" << _size)));
}

void DocumentSourceSample::doInjectExpressionContext() {
    _sortStage->injectExpressionContext(pExpCtx);
}

namespace {
const BSONObj randSortSpec = BSON("$rand" << BSON("$meta"
                                                  << "randVal"));
}  // namespace

intrusive_ptr<DocumentSource> DocumentSourceSample::createFromBson(
    BSONElement specElem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(28745, "the $sample stage specification must be an object", specElem.type() == Object);
    intrusive_ptr<DocumentSourceSample> sample(new DocumentSourceSample(expCtx));

    bool sizeSpecified = false;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();

        if (fieldName == "size") {
            uassert(28746, "size argument to $sample must be a number", elem.isNumber());
            uassert(28747, "size argument to $sample must not be negative", elem.numberLong() >= 0);
            sample->_size = elem.numberLong();
            sizeSpecified = true;
        } else {
            uasserted(28748, str::stream() << "unrecognized option to $sample: " << fieldName);
        }
    }
    uassert(28749, "$sample stage must specify a size", sizeSpecified);

    sample->_sortStage = DocumentSourceSort::create(expCtx, randSortSpec, sample->_size);

    return sample;
}

intrusive_ptr<DocumentSource> DocumentSourceSample::getShardSource() {
    return this;
}

intrusive_ptr<DocumentSource> DocumentSourceSample::getMergeSource() {
    // Just need to merge the pre-sorted documents by their random values.
    return DocumentSourceSort::create(pExpCtx, randSortSpec, _size);
}
}  // mongo
