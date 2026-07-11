// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/stemmer.h"

#include "mongo/util/assert_util.h"

#include <cstdlib>
#include <string>
#include <string_view>

#include <libstemmer.h>

namespace mongo::fts {

class Stemmer::Impl {
public:
    explicit Impl(const FTSLanguage* language) : _stemmer{_makeStemmer(language->str())} {}

    std::string_view stem(std::string_view word) const {
        auto st = _stemmer.get();
        if (!st)
            return word;
        auto sym =
            sb_stemmer_stem(st, reinterpret_cast<const sb_symbol*>(word.data()), word.size());
        invariant(sym);
        return std::string_view{reinterpret_cast<const char*>(sym),
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

std::string_view Stemmer::stem(std::string_view word) const {
    return _impl->stem(word);
}

}  // namespace mongo::fts
