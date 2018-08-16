/**
 * Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_backup_cursor.h"

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/util/log.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(backupCursor,
                         DocumentSourceBackupCursor::LiteParsed::parse,
                         DocumentSourceBackupCursor::createFromBson);

const char* DocumentSourceBackupCursor::kStageName = "$backupCursor";

DocumentSourceBackupCursor::DocumentSourceBackupCursor(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx),
      _backupCursorState(pExpCtx->mongoProcessInterface->openBackupCursor(pExpCtx->opCtx)) {}


DocumentSourceBackupCursor::~DocumentSourceBackupCursor() {
    try {
        pExpCtx->mongoProcessInterface->closeBackupCursor(pExpCtx->opCtx,
                                                          _backupCursorState.cursorId);
    } catch (DBException& exc) {
        severe() << exc.toStatus("Error closing a backup cursor.");
        fassertFailed(50909);
    }
}

DocumentSource::GetNextResult DocumentSourceBackupCursor::getNext() {
    pExpCtx->checkForInterrupt();

    if (_backupCursorState.preamble) {
        Document doc = _backupCursorState.preamble.get();
        _backupCursorState.preamble = boost::none;

        return std::move(doc);
    }

    if (!_backupCursorState.filenames.empty()) {
        Document doc = {{"filename", _backupCursorState.filenames.back()}};
        _backupCursorState.filenames.pop_back();

        return std::move(doc);
    }

    return GetNextResult::makeEOF();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceBackupCursor::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // The anticipated usage of a backup cursor: open the backup cursor, consume the results, copy
    // data off disk, close the backup cursor. The backup cursor must be successfully closed for
    // the data copied to be valid. Hence, the caller needs a way to keep the cursor open after
    // consuming the results, as well as the ability to send "heartbeats" to prevent the client
    // cursor manager from timing out the backup cursor. A backup cursor does consume resources;
    // in the event the calling process crashes, the cursors should eventually be timed out.
    pExpCtx->tailableMode = TailableModeEnum::kTailable;

    uassert(
        ErrorCodes::FailedToParse,
        str::stream() << kStageName << " value must be an object. Found: " << typeName(spec.type()),
        spec.type() == BSONType::Object);

    uassert(ErrorCodes::CannotBackup,
            str::stream() << kStageName << " cannot be executed against a MongoS.",
            !pExpCtx->inMongos && !pExpCtx->fromMongos && !pExpCtx->needsMerge);

    return new DocumentSourceBackupCursor(pExpCtx);
}

Value DocumentSourceBackupCursor::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(BSON(kStageName << 1));
}
}  // namespace mongo
