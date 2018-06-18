/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <sqlite3.h>
#include <string>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/mobile/mobile_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

#define SQLITE_STMT_TRACE() LOG(MOBILE_TRACE_LEVEL) << "MobileSE: SQLite Stmt ID:" << _id << " "

namespace mongo {

AtomicInt64 SqliteStatement::_nextID(0);

SqliteStatement::SqliteStatement(const MobileSession& session, const std::string& sqlQuery) {
    // Increment the global instance count and assign this instance an id.
    _id = _nextID.addAndFetch(1);
    SQLITE_STMT_TRACE() << "Preparing: " << sqlQuery;
    int status = sqlite3_prepare_v2(
        session.getSession(), sqlQuery.c_str(), sqlQuery.length() + 1, &_stmt, NULL);
    if (status == SQLITE_BUSY) {
        SQLITE_STMT_TRACE() << "Throwing writeConflictException, "
                            << "SQLITE_BUSY while preparing: " << sqlQuery;
        throw WriteConflictException();
    } else if (status != SQLITE_OK) {
        SQLITE_STMT_TRACE() << "Error while preparing: " << sqlQuery;
        std::string errMsg = "sqlite3_prepare_v2 failed: ";
        errMsg += sqlite3_errstr(status);
        uasserted(ErrorCodes::UnknownError, errMsg);
    }
}

SqliteStatement::~SqliteStatement() {
    int status = sqlite3_finalize(_stmt);
    fassert(37053, status == _exceptionStatus);
}

void SqliteStatement::bindInt(int paramIndex, int64_t intValue) {
    // SQLite bind methods begin paramater indexes at 1 rather than 0.
    int status = sqlite3_bind_int64(_stmt, paramIndex + 1, intValue);
    checkStatus(status, SQLITE_OK, "sqlite3_bind");
}

void SqliteStatement::bindBlob(int paramIndex, const void* data, int len) {
    // SQLite bind methods begin paramater indexes at 1 rather than 0.
    int status = sqlite3_bind_blob(_stmt, paramIndex + 1, data, len, SQLITE_STATIC);
    checkStatus(status, SQLITE_OK, "sqlite3_bind");
}

void SqliteStatement::bindText(int paramIndex, const char* data, int len) {
    // SQLite bind methods begin paramater indexes at 1 rather than 0.
    int status = sqlite3_bind_text(_stmt, paramIndex + 1, data, len, SQLITE_STATIC);
    checkStatus(status, SQLITE_OK, "sqlite3_bind");
}

void SqliteStatement::clearBindings() {
    int status = sqlite3_clear_bindings(_stmt);
    checkStatus(status, SQLITE_OK, "sqlite3_clear_bindings");
}

int SqliteStatement::step(int desiredStatus) {
    int status = sqlite3_step(_stmt);

    // A non-negative desiredStatus indicates that checkStatus should assert that the returned
    // status is equivalent to the desired status.
    if (desiredStatus >= 0) {
        checkStatus(status, desiredStatus, "sqlite3_step");
    }

    char* full_stmt = sqlite3_expanded_sql(_stmt);
    SQLITE_STMT_TRACE() << sqliteStatusToStr(status) << " - on stepping: " << full_stmt;
    sqlite3_free(full_stmt);

    return status;
}

int64_t SqliteStatement::getColInt(int colIndex) {
    return sqlite3_column_int64(_stmt, colIndex);
}

const void* SqliteStatement::getColBlob(int colIndex) {
    return sqlite3_column_blob(_stmt, colIndex);
}

int64_t SqliteStatement::getColBytes(int colIndex) {
    return sqlite3_column_bytes(_stmt, colIndex);
}

const void* SqliteStatement::getColText(int colIndex) {
    return sqlite3_column_text(_stmt, colIndex);
}

void SqliteStatement::execQuery(MobileSession* session, const std::string& query) {
    LOG(MOBILE_TRACE_LEVEL) << "MobileSE: SQLite sqlite3_exec: " << query;

    char* errMsg = NULL;
    int status = sqlite3_exec(session->getSession(), query.c_str(), NULL, NULL, &errMsg);

    if (status == SQLITE_BUSY || status == SQLITE_LOCKED) {
        LOG(MOBILE_TRACE_LEVEL) << "MobileSE: " << (status == SQLITE_BUSY ? "Busy" : "Locked")
                                << " - Throwing WriteConflictException on sqlite3_exec: " << query;
        throw WriteConflictException();
    }

    // The only return value from sqlite3_exec in a success case is SQLITE_OK.
    checkStatus(status, SQLITE_OK, "sqlite3_exec", errMsg);

    // When the error message is not NULL, it is allocated through sqlite3_malloc and must be freed
    // before exiting the method. If the error message is NULL, sqlite3_free is a no-op.
    sqlite3_free(errMsg);
}

void SqliteStatement::reset() {
    int status = sqlite3_reset(_stmt);
    checkStatus(status, SQLITE_OK, "sqlite3_reset");
}

}  // namespace mongo
