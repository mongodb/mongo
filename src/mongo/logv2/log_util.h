/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

namespace mongo::logv2 {
/**
 * Rotates the log files.  Returns true if all logs rotate successfully.
 *
 * renameFiles - true means we rename files, false means we expect the file to be renamed
 *               externally
 *
 * logrotate on *nix systems expects us not to rename the file, it is expected that the program
 * simply open the file again with the same name.
 * We expect logrotate to rename the existing file before we rotate, and so the next open
 * we do should result in a file create.
 */
bool rotateLogs(bool renameFiles);

/**
 * Returns true if system logs should be redacted.
 */
bool shouldRedactLogs();

/**
 * Set the 'redact' mode of the server.
 */
void setShouldRedactLogs(bool enabled);
}  // namespace mongo::logv2
