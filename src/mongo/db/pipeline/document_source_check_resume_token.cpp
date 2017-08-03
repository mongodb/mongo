/**
 * Copyright (C) 2017 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_check_resume_token.h"

using boost::intrusive_ptr;
namespace mongo {
const char* DocumentSourceCheckResumeToken::getSourceName() const {
    return "$_checkResumeToken";
}

Value DocumentSourceCheckResumeToken::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    // This stage is created by the DocumentSourceChangeNotification stage, so serializing it here
    // would result in it being created twice.
    return Value();
}

intrusive_ptr<DocumentSourceCheckResumeToken> DocumentSourceCheckResumeToken::create(
    const intrusive_ptr<ExpressionContext>& expCtx, DocumentSourceCheckResumeTokenSpec spec) {
    return new DocumentSourceCheckResumeToken(expCtx, std::move(spec));
}

DocumentSourceCheckResumeToken::DocumentSourceCheckResumeToken(
    const intrusive_ptr<ExpressionContext>& expCtx, DocumentSourceCheckResumeTokenSpec spec)
    : DocumentSource(expCtx), _token(spec.getResumeToken()), _seenDoc(false) {}

DocumentSource::GetNextResult DocumentSourceCheckResumeToken::getNext() {
    pExpCtx->checkForInterrupt();

    auto nextInput = pSource->getNext();
    uassert(40584,
            "resume of change notification was not possible, as no change data was found. ",
            _seenDoc || !nextInput.isEOF());

    if (_seenDoc || !nextInput.isAdvanced())
        return nextInput;

    _seenDoc = true;
    auto doc = nextInput.getDocument();

    ResumeToken receivedToken(doc["_id"]);
    uassert(
        40585,
        str::stream()
            << "resume of change notification was not possible, as the resume token was not found. "
            << receivedToken.toDocument().toString(),
        receivedToken == _token);
    // Don't return the document which has the token; the user has already seen it.
    return pSource->getNext();
}

}  // namespace mongo
