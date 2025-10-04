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

#include "mongo/db/fts/stemmer.h"

#include "mongo/util/assert_util.h"

#include <cstdlib>
#include <string>

#include <libstemmer.h>

namespace mongo::fts {

class Stemmer::Impl {
public:
    explicit Impl(const FTSLanguage* language) : _stemmer{_makeStemmer(language->str())} {}

    StringData stem(StringData word) const {
        auto st = _stemmer.get();
        if (!st)
            return word;
        auto sym =
            sb_stemmer_stem(st, reinterpret_cast<const sb_symbol*>(word.data()), word.size());
        invariant(sym);
        return StringData{reinterpret_cast<const char*>(sym),
                          static_cast<size_t>(sb_stemmer_length(st))};
    }

private:
    struct SbStemmerDeleter {
        void operator()(sb_stemmer* p) const {
            sb_stemmer_delete(p);
        }
    };

    static std::unique_ptr<sb_stemmer, SbStemmerDeleter> _makeStemmer(const std::string& lang) {
        if (lang == "none")
            return nullptr;
        return {sb_stemmer_new(lang.c_str(), "UTF_8"), {}};
    }

    std::unique_ptr<sb_stemmer, SbStemmerDeleter> _stemmer;
};

Stemmer::Stemmer(const FTSLanguage* language) : _impl{std::make_unique<Impl>(language)} {}

Stemmer::~Stemmer() = default;

StringData Stemmer::stem(StringData word) const {
    return _impl->stem(word);
}

}  // namespace mongo::fts
