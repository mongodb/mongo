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

#include "mongo/db/fts/fts_language.h"

#include <algorithm>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/fts/fts_basic_phrase_matcher.h"
#include "mongo/db/fts/fts_basic_tokenizer.h"
#include "mongo/db/fts/fts_unicode_phrase_matcher.h"
#include "mongo/db/fts/fts_unicode_tokenizer.h"
#include "mongo/util/assert_util.h"

namespace mongo::fts {
namespace {

using namespace fmt::literals;

struct CaseInsensitiveLess {
    template <typename T>
    bool operator()(const T& a, const T& b) const {
        using std::begin;
        using std::end;
        return std::lexicographical_compare(begin(a), end(a), begin(b), end(b), [](int a, int b) {
            return tolower(a) < tolower(b);
        });
    }
};

template <TextIndexVersion ver>
class LanguageRegistry {
public:
    // For V3 and above, use UnicodeFTSLanguage.
    using LanguageType =
        std::conditional_t<(ver >= TEXT_INDEX_VERSION_3), UnicodeFTSLanguage, BasicFTSLanguage>;

    // For V2 and above, language names are case-insensitive.
    using KeyCompare =
        std::conditional_t<(ver >= TEXT_INDEX_VERSION_2), CaseInsensitiveLess, std::less<>>;

    void add(StringData name, StringData alias = {}) {
        auto p = std::make_shared<const LanguageType>(std::string{name});
        _map[name.toString()] = p;
        if (!alias.empty()) {
            _map[alias.toString()] = p;
        }
    }

    StatusWith<const LanguageType*> make(StringData langName) const {
        std::string nameStr{langName};
        auto it = _map.find(nameStr);
        if (it == _map.end()) {
            if constexpr (ver == TEXT_INDEX_VERSION_1) {
                // v1 treats unrecognized language strings as "none".
                return _map.at("none").get();
            } else {
                // v2 and above reject unrecognized language strings.
                return {ErrorCodes::BadValue,
                        R"(unsupported language: "{}" for text index version {})"_format(langName,
                                                                                         ver)};
            }
        }
        return it->second.get();
    }

private:
    std::map<std::string, std::shared_ptr<const LanguageType>, KeyCompare> _map;
};

// template <TextIndexVersion ver>
// LanguageRegistry<ver> languageRegistry;

template <TextIndexVersion ver>
const LanguageRegistry<ver>& getLanguageRegistry() {
    static const auto instance = [] {
        if constexpr (ver == TEXT_INDEX_VERSION_1) {
            // Snowball language modules for TEXT_INDEX_VERSION_1.  Note that only the full names
            // are recognized by the StopWords class (as such, the language string "dan" in
            // TEXT_INDEX_VERSION_1 will generate the Danish stemmer and the empty stopword list).
            struct {
                StringData name;
            } static constexpr kLanguages[] = {
                {"none"_sd},      {"da"_sd},      {"dan"_sd},      {"danish"_sd},
                {"de"_sd},        {"deu"_sd},     {"dut"_sd},      {"dutch"_sd},
                {"en"_sd},        {"eng"_sd},     {"english"_sd},  {"es"_sd},
                {"esl"_sd},       {"fi"_sd},      {"fin"_sd},      {"finnish"_sd},
                {"fr"_sd},        {"fra"_sd},     {"fre"_sd},      {"french"_sd},
                {"ger"_sd},       {"german"_sd},  {"hu"_sd},       {"hun"_sd},
                {"hungarian"_sd}, {"it"_sd},      {"ita"_sd},      {"italian"_sd},
                {"nl"_sd},        {"nld"_sd},     {"no"_sd},       {"nor"_sd},
                {"norwegian"_sd}, {"por"_sd},     {"porter"_sd},   {"portuguese"_sd},
                {"pt"_sd},        {"ro"_sd},      {"romanian"_sd}, {"ron"_sd},
                {"ru"_sd},        {"rum"_sd},     {"rus"_sd},      {"russian"_sd},
                {"spa"_sd},       {"spanish"_sd}, {"sv"_sd},       {"swe"_sd},
                {"swedish"_sd},   {"tr"_sd},      {"tur"_sd},      {"turkish"_sd},
            };
            auto registry = new LanguageRegistry<ver>;
            for (auto&& spec : kLanguages) {
                registry->add(spec.name);
            }
            return registry;
        } else if constexpr (ver == TEXT_INDEX_VERSION_2 || ver == TEXT_INDEX_VERSION_3) {
            struct {
                StringData name;   // - lower case string name
                StringData alias;  // - language alias (if nonempty)
            } static constexpr kLanguages[] = {
                {"none"_sd, {}},
                {"danish"_sd, "da"_sd},
                {"dutch"_sd, "nl"_sd},
                {"english"_sd, "en"_sd},
                {"finnish"_sd, "fi"_sd},
                {"french"_sd, "fr"_sd},
                {"german"_sd, "de"_sd},
                {"hungarian"_sd, "hu"_sd},
                {"italian"_sd, "it"_sd},
                {"norwegian"_sd, "nb"_sd},
                {"portuguese"_sd, "pt"_sd},
                {"romanian"_sd, "ro"_sd},
                {"russian"_sd, "ru"_sd},
                {"spanish"_sd, "es"_sd},
                {"swedish"_sd, "sv"_sd},
                {"turkish"_sd, "tr"_sd},
            };
            auto registry = new LanguageRegistry<ver>;
            for (auto&& spec : kLanguages) {
                registry->add(spec.name, spec.alias);
            }
            return registry;
        } else {
            uasserted(ErrorCodes::BadValue, "invalid text index version");
        }
    }();
    return *instance;
}

}  // namespace

StatusWithFTSLanguage FTSLanguage::make(StringData langName, TextIndexVersion textIndexVersion) {
    switch (textIndexVersion) {
        case TEXT_INDEX_VERSION_1:
            return getLanguageRegistry<TEXT_INDEX_VERSION_1>().make(langName);
        case TEXT_INDEX_VERSION_2:
            return getLanguageRegistry<TEXT_INDEX_VERSION_2>().make(langName);
        case TEXT_INDEX_VERSION_3:
            return getLanguageRegistry<TEXT_INDEX_VERSION_3>().make(langName);
        case TEXT_INDEX_VERSION_INVALID:
            break;
    }
    return {ErrorCodes::BadValue, "invalid TextIndexVersion"};
}

std::unique_ptr<FTSTokenizer> BasicFTSLanguage::createTokenizer() const {
    return std::make_unique<BasicFTSTokenizer>(this);
}

std::unique_ptr<FTSTokenizer> UnicodeFTSLanguage::createTokenizer() const {
    return std::make_unique<UnicodeFTSTokenizer>(this);
}

}  // namespace mongo::fts
