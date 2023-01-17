/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

namespace mongo {

struct ImportOptions {
    enum class ImportCollectionUUIDOption { kKeepOld, kGenerateNew };

    // Dictates the timestamp rule for importing files.
    enum class ImportTimestampRule {
        /* Allow importing files with their checkpoint timestamp smaller than
         * or equal to the oldest timestamp.
         *
         * This is the default behavior.
         */
        kOldest,
        /* Allow importing files with their checkpoint timestamp smaller than
         * or equal to the stable timestamp.
         *
         * Caution: Snapshot reads of historical data will not work  with this option.
         */
        kStable
    };

    ImportOptions(ImportCollectionUUIDOption importCollectionUUIDOption)
        : importCollectionUUIDOption(importCollectionUUIDOption) {}

    ImportCollectionUUIDOption importCollectionUUIDOption;
    ImportTimestampRule importTimestampRule = ImportTimestampRule::kOldest;

    // Determines if we need to set new rand for this node to avoid ident collision in future due to
    // this collection & it's associated index import.
    bool skipIdentCollisionCheck = false;
};
}  // namespace mongo
