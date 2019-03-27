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

#pragma once

#include <string>


namespace mongo {
namespace unittest {

/**
 * An RAII temporary directory that deletes itself and all contents files on scope exit.
 */
class TempDir {
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

public:
    /**
     * Creates a new unique temporary directory.
     *
     * Throws if this fails for any reason, such as bad permissions.
     *
     * The leaf of the directory path will start with namePrefix and have
     * unspecified characters added to ensure uniqueness.
     *
     * namePrefix must not contain either / or \
     */
    explicit TempDir(const std::string& namePrefix);

    /**
     * Delete the directory and all contents.
     *
     * This only does best-effort. In particular no new files should be created in the directory
     * once the TempDir goes out of scope. Any errors are logged and ignored.
     */
    ~TempDir();

    const std::string& path() {
        return _path;
    }

    /**
     * Set the path where TempDir() will create temporary directories. This is a workaround
     * for situations where you might want to log, but you've not yet run the MONGO_INITIALIZERs,
     * and should be removed if ever command line parsing is seperated from MONGO_INITIALIZERs.
     */
    static void setTempPath(std::string tempPath);

private:
    std::string _path;
};
}  // namespace unittest
}  // namespace mongo
