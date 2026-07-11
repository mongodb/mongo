// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mongo {

namespace fts {

class FTSTokenizer;

class FTSQueryImpl final : public FTSQuery {
public:
    // Uses `std::less<>` to allow heterogeneous lookup
    using StringSet = std::set<std::string, std::less<>>;

    Status parse(TextIndexVersion textIndexVersion) final;

    std::unique_ptr<FTSQuery> clone() const final;

    const StringSet& getPositiveTerms() const {
        return _positiveTerms;
    }
    const StringSet& getNegatedTerms() const {
        return _negatedTerms;
    }
    const std::vector<std::string>& getPositivePhr() const {
        return _positivePhrases;
    }
    const std::vector<std::string>& getNegatedPhr() const {
        return _negatedPhrases;
    }

    const StringSet& getTermsForBounds() const {
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

    size_t getApproximateSize() const final;

private:
    void _addTerms(FTSTokenizer* tokenizer, const std::string& tokens, bool negated);

    StringSet _positiveTerms;
    StringSet _negatedTerms;
    std::vector<std::string> _positivePhrases;
    std::vector<std::string> _negatedPhrases;
    StringSet _termsForBounds;
};
}  // namespace fts
}  // namespace mongo
