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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/tee_buffer.h"

namespace mongo {

class Document;
struct ExpressionContext;
class Value;

/**
 * This stage acts as a proxy between a pipeline within a $facet stage and the buffer of incoming
 * documents held in a TeeBuffer stage. It will simply open an iterator on the TeeBuffer stage, and
 * answer calls to getNext() by advancing said iterator.
 */
class DocumentSourceTeeConsumer : public DocumentSource {
public:
    static boost::intrusive_ptr<DocumentSourceTeeConsumer> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<TeeBuffer>& bufferSource);

    void dispose() final;
    boost::optional<Document> getNext() final;

    /**
     * Returns SEE_NEXT, since it requires no fields, and changes nothing about the documents.
     */
    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return GetDepsReturn::SEE_NEXT;
    }

    Value serialize(bool explain = false) const final;

private:
    DocumentSourceTeeConsumer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const boost::intrusive_ptr<TeeBuffer>& bufferSource);

    bool _initialized = false;
    boost::intrusive_ptr<TeeBuffer> _bufferSource;
    TeeBuffer::const_iterator _iterator;
};
}  // namespace mongo
