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

#pragma once

#include <sqlite3.h>
#include <string>

#include "mongo/db/storage/mobile/mobile_session.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * SqliteStatement is a wrapper around the sqlite3_stmt object. All calls to the SQLite API that
 * involve a sqlite_stmt object are made in this class.
 */
class SqliteStatement final {
public:
    /**
     * Creates and prepares a SQLite statement.
     */
    SqliteStatement(const MobileSession& session, const std::string& sqlQuery);

    /**
     * Finalizes the prepared statement.
     */
    ~SqliteStatement();

    /**
     * The various bind methods bind a value to the query parameter specified by paramIndex.
     *
     * @param paramIndex - zero-based index of a query parameter.
     */
    void bindInt(int paramIndex, int64_t intValue);

    void bindBlob(int paramIndex, const void* data, int len);

    void bindText(int paramIndex, const char* data, int len);

    void clearBindings();

    /**
     * Wraps sqlite3_step and returns the resulting status.
     *
     * @param desiredStatus - the desired return status of sqlite3_step. When desiredStatus is
     * non-negative, checkStatus compares desiredStatus with the returned status from sqlite3_step.
     * By default, checkStatus is ignored.
     */
    int step(int desiredStatus = -1);

    /**
     * The getCol methods wrap sqlite3_column methods and return the correctly typed values
     * stored in a retrieved query row[colIndex].
     *
     * @param colIndex - zero-based index of a column retrieved from a query row.
     */
    int64_t getColInt(int colIndex);

    const void* getColBlob(int colIndex);

    /**
     * Returns the number of bytes in a corresponding blob or string.
     */
    int64_t getColBytes(int colIndex);

    /**
     * Wraps sqlite3_column_text method and returns the text from the retrieved query row[colIndex].
     *
     * @param colIndex - zero-based index of a column retrieved from a query row.
     */
    const void* getColText(int colIndex);

    /**
     * Resets the statement to the first of the query result rows.
     */
    void reset();

    /**
     * Sets the last status on the prepared statement.
     */
    void setExceptionStatus(int status) {
        _exceptionStatus = status;
    }

    /**
     * A one step query execution that wraps sqlite3_prepare_v2(), sqlite3_step(), and
     * sqlite3_finalize().
     * None of the rows retrieved, if any, are saved before the query is finalized. Thus, this
     * method should not be used for read operations.
     */
    static void execQuery(MobileSession* session, const std::string& query);

    uint64_t _id;

private:
    static AtomicInt64 _nextID;
    sqlite3_stmt* _stmt;

    // If the most recent call to sqlite3_step on this statement returned an error, the error is
    // returned again when the statement is finalized. This is used to verify that the last error
    // code returned matches the finalize error code, if there is any.
    int _exceptionStatus = SQLITE_OK;
};
}  // namespace mongo
