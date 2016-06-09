/**
 * Copyright 2015 (c) MongoDB, Inc.
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

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceMock::DocumentSourceMock(std::deque<Document> docs)
    : DocumentSource(NULL), queue(std::move(docs)) {}

DocumentSourceMock::DocumentSourceMock(std::deque<Document> docs,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(expCtx), queue(std::move(docs)) {}

const char* DocumentSourceMock::getSourceName() const {
    return "mock";
}

Value DocumentSourceMock::serialize(bool explain) const {
    return Value(DOC(getSourceName() << Document()));
}

void DocumentSourceMock::dispose() {
    isDisposed = true;
}

intrusive_ptr<DocumentSourceMock> DocumentSourceMock::create(std::deque<Document> docs) {
    return new DocumentSourceMock(std::move(docs));
}

intrusive_ptr<DocumentSourceMock> DocumentSourceMock::create() {
    return new DocumentSourceMock(std::deque<Document>());
}

intrusive_ptr<DocumentSourceMock> DocumentSourceMock::create(const Document& doc) {
    std::deque<Document> docs = {doc};
    return new DocumentSourceMock(std::move(docs));
}

intrusive_ptr<DocumentSourceMock> DocumentSourceMock::create(const char* json) {
    return create(Document(fromjson(json)));
}

intrusive_ptr<DocumentSourceMock> DocumentSourceMock::create(
    const std::initializer_list<const char*>& jsons) {
    std::deque<Document> docs;
    for (auto&& json : jsons) {
        docs.push_back(Document(fromjson(json)));
    }
    return new DocumentSourceMock(std::move(docs));
}

boost::optional<Document> DocumentSourceMock::getNext() {
    invariant(!isDisposed);
    invariant(!isDetachedFromOpCtx);

    if (queue.empty()) {
        return {};
    }

    Document doc = std::move(queue.front());
    queue.pop_front();
    return {std::move(doc)};
}
}
