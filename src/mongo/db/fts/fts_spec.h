// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/stop_words.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {

namespace fts {

extern const double MAX_WEIGHT;
extern const double MAX_WORD_WEIGHT;
extern const double DEFAULT_WEIGHT;

// This type is used in index_catalog_entry_helpers::computeUpdateIndexData() to create an iterator.
// Said iterator could be replaced with auto in order to avoid exposing this typedef.
[[MONGO_MOD_NEEDS_REPLACEMENT]] typedef std::map<std::string, double> Weights;  // TODO cool map
typedef stdx::unordered_map<std::string, double> TermFrequencyMap;

struct ScoreHelperStruct {
    ScoreHelperStruct() : freq(0), count(0), exp(0) {}
    double freq;
    double count;
    double exp;
};
typedef StringMap<ScoreHelperStruct> ScoreHelperMap;

class [[MONGO_MOD_PUBLIC]] FTSSpec {
    struct Tools {
        Tools(const FTSLanguage& _language, const Stemmer* _stemmer, const StopWords* _stopwords)
            : language(_language), stemmer(_stemmer), stopwords(_stopwords) {}

        const FTSLanguage& language;
        const Stemmer* stemmer;
        const StopWords* stopwords;
    };

public:
    FTSSpec(const BSONObj& indexInfo);

    bool wildcard() const {
        return _wildcard;
    }
    const FTSLanguage& defaultLanguage() const {
        return *_defaultLanguage;
    }
    const std::string& languageOverrideField() const {
        return _languageOverrideField;
    }

    size_t numExtraBefore() const {
        return _extraBefore.size();
    }
    const std::string& extraBefore(unsigned i) const {
        return _extraBefore[i];
    }

    size_t numExtraAfter() const {
        return _extraAfter.size();
    }
    const std::string& extraAfter(unsigned i) const {
        return _extraAfter[i];
    }

    /**
     * Calculates term/score pairs for a BSONObj as applied to this spec.
     * @arg obj  document to traverse; can be a subdocument or array
     * @arg term_freqs  output parameter to store (term,score) results
     */
    void scoreDocument(const BSONObj& obj, TermFrequencyMap* term_freqs) const;

    /**
     * given a query, pulls out the pieces (in order) that go in the index first
     */
    Status getIndexPrefix(const BSONObj& filter, BSONObj* out) const;

    const Weights& weights() const {
        return _weights;
    }
    static StatusWith<BSONObj> fixSpec(const BSONObj& spec);

    /**
     * Returns text index version.
     */
    TextIndexVersion getTextIndexVersion() const {
        return _textIndexVersion;
    }

    size_t getApproximateSize() const;

private:
    //
    // Helper methods.  Invoked for TEXT_INDEX_VERSION_2 spec objects only.
    //

    /**
     * Calculate the term scores for 'raw' and update 'term_freqs' with the result.  Parses
     * 'raw' using 'tools', and weights term scores based on 'weight'.
     */
    void _scoreStringV2(FTSTokenizer* tokenizer,
                        std::string_view raw,
                        TermFrequencyMap* term_freqs,
                        double weight) const;

public:
    /**
     * Get the language override for the given BSON doc.  If no language override is
     * specified, returns currentLanguage.
     */
    const FTSLanguage* _getLanguageToUseV2(const BSONObj& userDoc,
                                           const FTSLanguage* currentLanguage) const;

private:
    //
    // Deprecated helper methods.  Invoked for TEXT_INDEX_VERSION_1 spec objects only.
    //

    void _scoreStringV1(const Tools& tools,
                        std::string_view raw,
                        TermFrequencyMap* docScores,
                        double weight) const;

    bool _weightV1(std::string_view field, double* out) const;

    void _scoreRecurseV1(const Tools& tools,
                         const BSONObj& obj,
                         TermFrequencyMap* term_freqs) const;

    void _scoreDocumentV1(const BSONObj& obj, TermFrequencyMap* term_freqs) const;

    const FTSLanguage& _getLanguageToUseV1(const BSONObj& userDoc) const;

    static StatusWith<BSONObj> _fixSpecV1(const BSONObj& spec);

    //
    // Instance variables.
    //

    TextIndexVersion _textIndexVersion;

    const FTSLanguage* _defaultLanguage;
    std::string _languageOverrideField;
    bool _wildcard;

    // mapping : fieldname -> weight
    Weights _weights;

    // Prefix compound key - used to partition search index
    std::vector<std::string> _extraBefore;

    // Suffix compound key - used for covering index behavior
    std::vector<std::string> _extraAfter;
};
}  // namespace fts
}  // namespace mongo
