/**
 * Copyright (c) 2014 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <set>
#include <string>

#include "mongo/db/pipeline/document.h"

namespace mongo {
class ParsedDeps;

/**
 * This struct allows components in an agg pipeline to report what they need from their input.
 */
struct DepsTracker {
    /**
     * Represents what metadata is available on documents that are input to the pipeline.
     */
    enum MetadataAvailable { kNoMetadata = 0, kTextScore = 1 };

    DepsTracker(MetadataAvailable metadataAvailable = kNoMetadata)
        : needWholeDocument(false), _metadataAvailable(metadataAvailable), _needTextScore(false) {}

    /**
     * Returns a projection object covering the dependencies tracked by this class.
     */
    BSONObj toProjection() const;

    boost::optional<ParsedDeps> toParsedDeps() const;

    std::set<std::string> fields;  // names of needed fields in dotted notation
    bool needWholeDocument;        // if true, ignore fields and assume the whole document is needed


    bool hasNoRequirements() const {
        return fields.empty() && !needWholeDocument && !_needTextScore;
    }

    MetadataAvailable getMetadataAvailable() const {
        return _metadataAvailable;
    }

    bool isTextScoreAvailable() const {
        return _metadataAvailable & MetadataAvailable::kTextScore;
    }

    bool getNeedTextScore() const {
        return _needTextScore;
    }

    void setNeedTextScore(bool needTextScore) {
        if (needTextScore && !isTextScoreAvailable()) {
            uasserted(
                40218,
                "pipeline requires text score metadata, but there is no text score available");
        }
        _needTextScore = needTextScore;
    }

private:
    MetadataAvailable _metadataAvailable;
    bool _needTextScore;
};

/**
 * This class is designed to quickly extract the needed fields from a BSONObj into a Document.
 * It should only be created by a call to DepsTracker::ParsedDeps
 */
class ParsedDeps {
public:
    Document extractFields(const BSONObj& input) const;

private:
    friend struct DepsTracker;  // so it can call constructor
    explicit ParsedDeps(const Document& fields) : _fields(fields) {}

    Document _fields;
};
}
