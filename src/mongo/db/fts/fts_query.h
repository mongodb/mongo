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
