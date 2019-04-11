/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/util/log.h"
#include "mongo/util/str.h"

#include <sqlite3.h>

#include "mongo/db/storage/mobile/mobile_options.h"
#include "mongo/db/storage/mobile/mobile_recovery_unit.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/mobile/mobile_util.h"

namespace mongo {
namespace embedded {

using std::string;

Status sqliteRCToStatus(int retCode, const char* prefix) {
    str::stream s;
    if (prefix)
        s << prefix << " ";
    s << retCode << ": " << sqlite3_errstr(retCode);

    switch (retCode) {
        case SQLITE_OK:
            return Status::OK();
        case SQLITE_INTERNAL:
            return Status(ErrorCodes::InternalError, s);
        case SQLITE_PERM:
            return Status(ErrorCodes::Unauthorized, s);
        case SQLITE_BUSY:
            return Status(ErrorCodes::LockBusy, s);
        case SQLITE_LOCKED:
            return Status(ErrorCodes::LockBusy, s);
        case SQLITE_NOMEM:
            return Status(ErrorCodes::ExceededMemoryLimit, s);
        case SQLITE_READONLY:
            return Status(ErrorCodes::Unauthorized, s);
        case SQLITE_INTERRUPT:
            return Status(ErrorCodes::Interrupted, s);
        case SQLITE_CANTOPEN:
            return Status(ErrorCodes::FileOpenFailed, s);
        case SQLITE_PROTOCOL:
            return Status(ErrorCodes::ProtocolError, s);
        case SQLITE_MISMATCH:
            return Status(ErrorCodes::TypeMismatch, s);
        case SQLITE_MISUSE:
            return Status(ErrorCodes::BadValue, s);
        case SQLITE_NOLFS:
            return Status(ErrorCodes::CommandNotSupported, s);
        case SQLITE_AUTH:
            return Status(ErrorCodes::AuthenticationFailed, s);
        case SQLITE_FORMAT:
            return Status(ErrorCodes::UnsupportedFormat, s);
        case SQLITE_RANGE:
            return Status(ErrorCodes::BadValue, s);
        case SQLITE_NOTADB:
            return Status(ErrorCodes::FileOpenFailed, s);
        default:
            return Status(ErrorCodes::UnknownError, s);
    }
}

const char* sqliteStatusToStr(int retStatus) {
    const char* msg = NULL;

    switch (retStatus) {
        case SQLITE_OK:
            msg = "SQLITE_OK";
            break;
        case SQLITE_ERROR:
            msg = "SQLITE_ERROR";
            break;
        case SQLITE_BUSY:
            msg = "SQLITE_BUSY";
            break;
        case SQLITE_LOCKED:
            msg = "SQLITE_LOCKED";
            break;
        case SQLITE_NOTFOUND:
            msg = "SQLITE_NOTFOUND";
            break;
        case SQLITE_FULL:
            msg = "SQLITE_FULL";
            break;
        case SQLITE_MISUSE:
            msg = "SQLITE_MISUSE";
            break;
        case SQLITE_ROW:
            msg = "SQLITE_ROW";
            break;
        case SQLITE_DONE:
            msg = "SQLITE_DONE";
            break;
        default:
            msg = "Status not converted";
            break;
    }
    return msg;
}

void checkStatus(int retStatus, int desiredStatus, const char* fnName, const char* errMsg) {
    if (retStatus != desiredStatus) {
        std::stringstream s;
        s << fnName << " failed with return status " << sqlite3_errstr(retStatus);

        if (errMsg) {
            s << "------ Error Message: " << errMsg;
        }

        severe() << s.str();
        fassertFailed(37000);
    }
}

/**
 * Helper to add and log errors for validate.
 */
void validateLogAndAppendError(ValidateResults* results, const std::string& errMsg) {
    error() << "validate found error: " << errMsg;
    results->errors.push_back(errMsg);
    results->valid = false;
}

void doValidate(OperationContext* opCtx, ValidateResults* results) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    try {
        SqliteStatement validateStmt(*session, "PRAGMA integrity_check;");

        int status;
        // By default, the integrity check returns the first 100 errors found.
        while ((status = validateStmt.step()) == SQLITE_ROW) {
            std::string errMsg(validateStmt.getColText(0));

            if (errMsg == "ok") {
                // If the first message returned is "ok", the integrity check passed without
                // finding any corruption.
                continue;
            }

            validateLogAndAppendError(results, errMsg);
        }

        if (status == SQLITE_CORRUPT) {
            uasserted(ErrorCodes::UnknownError, sqlite3_errstr(status));
        }
        checkStatus(status, SQLITE_DONE, "sqlite3_step");

    } catch (const DBException& e) {
        // SQLite statement may fail to prepare or execute correctly if the file is corrupted.
        std::string errMsg = "database file is corrupt - " + e.toString();
        validateLogAndAppendError(results, errMsg);
    }
}

void configureSession(sqlite3* session, const MobileOptions& options) {
    auto executePragma = [session](auto pragma, auto value) {
        SqliteStatement::execQuery(session, "PRAGMA ", pragma, " = ", value, ";");
        LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE session configuration: " << pragma << " = " << value;
    };
    // We don't manually use VACUUM so set incremental(2) mode to reclaim space
    // This need to be set the first thing we do, before any internal tables are created.
    executePragma("auto_vacuum"_sd, "incremental"_sd);

    // Set SQLite in Write-Ahead Logging mode. https://sqlite.org/wal.html
    executePragma("journal_mode"_sd, "WAL"_sd);

    // synchronous = NORMAL(1) is recommended with WAL, but we allow it to be overriden
    executePragma("synchronous"_sd, std::to_string(options.durabilityLevel));

    // Set full fsync on OSX (only supported there) to ensure durability
    executePragma("fullfsync"_sd, "1"_sd);

    // We just use SQLite as key-value store, so disable foreign keys
    executePragma("foreign_keys"_sd, "0"_sd);

    // Set some additional internal sizes for this session
    // Cache size described as KB should be set as negative number
    // https://sqlite.org/pragma.html#pragma_cache_size
    executePragma("cache_size"_sd, std::to_string(-static_cast<int32_t>(options.cacheSizeKB)));
    executePragma("mmap_size"_sd, std::to_string(options.mmapSizeKB * 1024));
    executePragma("journal_size_limit"_sd, std::to_string(options.journalSizeLimitKB * 1024));
}

}  // namespace embedded
}  // namespace mongo
