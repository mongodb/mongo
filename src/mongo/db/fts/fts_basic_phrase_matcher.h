/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/fts/fts_phrase_matcher.h"

namespace mongo {
namespace fts {

/**
 * A phrase matcher that looks for exact substring matches with optional ASCII-aware case
 * insensitivity. This phrase matcher does not implement the kDiacriticSensitive match option. All
 * operations are inherently diacritic sensitive.
 */
class BasicFTSPhraseMatcher final : public FTSPhraseMatcher {
    MONGO_DISALLOW_COPYING(BasicFTSPhraseMatcher);

public:
    BasicFTSPhraseMatcher() = default;

    bool phraseMatches(const std::string& phrase,
                       const std::string& haystack,
                       Options options) const override;
};

}  // namespace fts
}  // namespace mongo
