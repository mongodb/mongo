// fts_query_impl.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#include <set>
#include <string>
#include <vector>

#include "mongo/db/fts/fts_query.h"

namespace mongo {

namespace fts {

class FTSTokenizer;

class FTSQueryImpl final : public FTSQuery {
public:
    Status parse(TextIndexVersion textIndexVersion) final;

    std::unique_ptr<FTSQuery> clone() const final;

    const std::set<std::string>& getPositiveTerms() const {
        return _positiveTerms;
    }
    const std::set<std::string>& getNegatedTerms() const {
        return _negatedTerms;
    }
    const std::vector<std::string>& getPositivePhr() const {
        return _positivePhrases;
    }
    const std::vector<std::string>& getNegatedPhr() const {
        return _negatedPhrases;
    }

    const std::set<std::string>& getTermsForBounds() const {
        return _termsForBounds;
    }

    /**
     * Returns a BSON object with the following format:
     * {
     *   terms: <array of positive terms>,
     *   negatedTerms: <array of negative terms>,
     *   phrases: <array of positive phrases>,
     *   negatedPhrases: <array of negative phrases>
     * }
     */
    BSONObj toBSON() const;

private:
    void _addTerms(FTSTokenizer* tokenizer, const std::string& tokens, bool negated);

    std::set<std::string> _positiveTerms;
    std::set<std::string> _negatedTerms;
    std::vector<std::string> _positivePhrases;
    std::vector<std::string> _negatedPhrases;
    std::set<std::string> _termsForBounds;
};
}
}
