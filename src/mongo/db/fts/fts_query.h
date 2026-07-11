// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/fts/fts_util.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {
namespace fts {

/**
 * An FTSQuery represents a parsed text search query.
 */
class FTSQuery {
public:
    virtual ~FTSQuery() {}

    void setQuery(std::string query) {
        _query = std::move(query);
    }

    void setLanguage(std::string language) {
        _language = std::move(language);
    }

    void setCaseSensitive(bool caseSensitive) {
        _caseSensitive = caseSensitive;
    }

    void setDiacriticSensitive(bool diacriticSensitive) {
        _diacriticSensitive = diacriticSensitive;
    }

    const std::string& getQuery() const {
        return _query;
    }

    const std::string& getLanguage() const {
        return _language;
    }

    bool getCaseSensitive() const {
        return _caseSensitive;
    }

    bool getDiacriticSensitive() const {
        return _diacriticSensitive;
    }

    /**
     * Returns true iff '*this' and 'other' have the same unparsed form.
     */
    bool equivalent(const FTSQuery& other) const {
        return _query == other._query && _language == other._language &&
            _caseSensitive == other._caseSensitive &&
            _diacriticSensitive == other._diacriticSensitive;
    }

    /**
     * Parses the text search query. Before parsing, the FTSQuery needs to be initialized with
     * the set*() methods above.
     *
     * Returns Status::OK() if parsing was successful; returns an error Status otherwise.
     */
    virtual Status parse(TextIndexVersion textIndexVersion) = 0;

    /**
     * Returns a copy of this FTSQuery.
     */
    virtual std::unique_ptr<FTSQuery> clone() const = 0;

    virtual size_t getApproximateSize() const {
        return sizeof(FTSQuery) + _query.size() + 1 + _language.size() + 1;
    }

    /**
     * FTSQuery's hash function compatible with absl::Hash. Designed be consistent with
     * 'FTSQuery::equivalent()'.
     */
    template <typename H>
    friend H AbslHashValue(H h, const FTSQuery& ftsQuery) {
        return H::combine(std::move(h),
                          ftsQuery.getQuery(),
                          ftsQuery.getLanguage(),
                          ftsQuery.getCaseSensitive(),
                          ftsQuery.getDiacriticSensitive());
    }

private:
    std::string _query;
    std::string _language;
    bool _caseSensitive = false;
    bool _diacriticSensitive = false;
};

}  // namespace fts
}  // namespace mongo
