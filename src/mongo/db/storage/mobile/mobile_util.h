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

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"

#define MOBILE_LOG_LEVEL_LOW 2
#define MOBILE_LOG_LEVEL_HIGH 5
#define MOBILE_TRACE_LEVEL MOBILE_LOG_LEVEL_HIGH

namespace mongo {

/**
 * Converts SQLite return codes to MongoDB statuses.
 */
Status sqliteRCToStatus(int retCode, const char* prefix = NULL);

/**
 * Converts SQLite return codes to string equivalents.
 */
const char* sqliteStatusToStr(int retStatus);

/**
 * Checks if retStatus == desiredStatus; else calls fassert.
 */
void checkStatus(int retStatus, int desiredStatus, const char* fnName, const char* errMsg = NULL);

/**
 * Validate helper function to log an error and append the error to the results.
 */
void validateLogAndAppendError(ValidateResults* results, const std::string& errMsg);

/**
 * Checks if the database file is corrupt.
 */
void doValidate(OperationContext* opCtx, ValidateResults* results);

}  // namespace mongo
