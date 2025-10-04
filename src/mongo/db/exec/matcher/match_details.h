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

#include <memory>
#include <string>

namespace mongo {

/** Reports information about a match request. */
class MatchDetails {
public:
    MatchDetails();

    void resetOutput();

    // for debugging only
    std::string toString() const;

    // relating to whether or not we had to load the full record

    void setLoadedRecord(bool loadedRecord) {
        _loadedRecord = loadedRecord;
    }

    bool hasLoadedRecord() const {
        return _loadedRecord;
    }

    // this name is wrong

    bool needRecord() const {
        return _elemMatchKeyRequested;
    }

    // if we need to store the offset into an array where we found the match

    /** Request that an elemMatchKey be recorded. */
    void requestElemMatchKey() {
        _elemMatchKeyRequested = true;
    }

    bool hasElemMatchKey() const;
    std::string elemMatchKey() const;

    void setElemMatchKey(const std::string& elemMatchKey);

private:
    bool _loadedRecord;
    bool _elemMatchKeyRequested;
    std::unique_ptr<std::string> _elemMatchKey;
};
}  // namespace mongo
