//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/boundary.hpp>
#include <boost/locale/generator.hpp>
#include "../util/encoding.hpp"
#include "all_generator.hpp"
#include "cdata.hpp"
#include "icu_util.hpp"
#include "uconv.hpp"
#if BOOST_LOCALE_ICU_VERSION >= 5502
#    include <unicode/utext.h>
#endif
#include <memory>
#include <unicode/brkiter.h>
#include <unicode/rbbi.h>
#include <vector>

#ifdef BOOST_MSVC
#    pragma warning(disable : 4244) // 'argument' : conversion from 'int'
#    pragma warning(disable : 4267) // 'argument' : conversion from 'size_t'
#endif

#if BOOST_LOCALE_ICU_VERSION >= 5502
namespace std {
template<>
struct default_delete<UText> {
    using pointer = UText*;
    void operator()(pointer ptr) { utext_close(ptr); }
};
} // namespace std
#endif

namespace boost { namespace locale {
    namespace boundary { namespace impl_icu {

        using namespace boost::locale::impl_icu;

        index_type map_direct(boundary_type t, icu::BreakIterator* it, int reserve)
        {
            index_type indx;
            indx.reserve(reserve);
#if U_ICU_VERSION_MAJOR_NUM >= 52
            icu::BreakIterator* rbbi = it;
#else
            icu::RuleBasedBreakIterator* rbbi = icu_cast<icu::RuleBasedBreakIterator>(it);
#endif

            indx.push_back(break_info());
            it->first();
            int pos = 0;
            while((pos = it->next()) != icu::BreakIterator::DONE) {
                indx.push_back(break_info(pos));
                // Character does not have any specific break types
                if(t != character && rbbi) {
                    std::vector<int32_t> buffer;
                    int32_t membuf[8] = {0}; // try not to use memory allocation if possible
                    int32_t* buf = membuf;

                    UErrorCode err = U_ZERO_ERROR;
                    int n = rbbi->getRuleStatusVec(buf, 8, err);

                    if(err == U_BUFFER_OVERFLOW_ERROR) {
                        buffer.resize(n, 0);
                        buf = buffer.data();
                        n = rbbi->getRuleStatusVec(buf, buffer.size(), err);
                    }

                    check_and_throw_icu_error(err);

                    for(int i = 0; i < n; i++) {
                        switch(t) {
                            case word:
                                if(UBRK_WORD_NONE <= buf[i] && buf[i] < UBRK_WORD_NONE_LIMIT)
                                    indx.back().rule |= word_none;
                                else if(UBRK_WORD_NUMBER <= buf[i] && buf[i] < UBRK_WORD_NUMBER_LIMIT)
                                    indx.back().rule |= word_number;
                                else if(UBRK_WORD_LETTER <= buf[i] && buf[i] < UBRK_WORD_LETTER_LIMIT)
                                    indx.back().rule |= word_letter;
                                else if(UBRK_WORD_KANA <= buf[i] && buf[i] < UBRK_WORD_KANA_LIMIT)
                                    indx.back().rule |= word_kana;
                                else if(UBRK_WORD_IDEO <= buf[i] && buf[i] < UBRK_WORD_IDEO_LIMIT)
                                    indx.back().rule |= word_ideo;
                                break;

                            case line:
                                if(UBRK_LINE_SOFT <= buf[i] && buf[i] < UBRK_LINE_SOFT_LIMIT)
                                    indx.back().rule |= line_soft;
                                else if(UBRK_LINE_HARD <= buf[i] && buf[i] < UBRK_LINE_HARD_LIMIT)
                                    indx.back().rule |= line_hard;
                                break;

                            case sentence:
                                if(UBRK_SENTENCE_TERM <= buf[i] && buf[i] < UBRK_SENTENCE_TERM_LIMIT)
                                    indx.back().rule |= sentence_term;
                                else if(UBRK_SENTENCE_SEP <= buf[i] && buf[i] < UBRK_SENTENCE_SEP_LIMIT)
                                    indx.back().rule |= sentence_sep;
                                break;
                            case character: BOOST_UNREACHABLE_RETURN(0);
                        }
                    }
                } else
                    indx.back().rule |= character_any; // Basic mark... for character
            }
            return indx;
        }

        std::unique_ptr<icu::BreakIterator> get_iterator(boundary_type t, const icu::Locale& loc)
        {
            UErrorCode err = U_ZERO_ERROR;
            std::unique_ptr<icu::BreakIterator> bi;
            switch(t) {
                case character: bi.reset(icu::BreakIterator::createCharacterInstance(loc, err)); break;
                case word: bi.reset(icu::BreakIterator::createWordInstance(loc, err)); break;
                case sentence: bi.reset(icu::BreakIterator::createSentenceInstance(loc, err)); break;
                case line: bi.reset(icu::BreakIterator::createLineInstance(loc, err)); break;
            }
            check_and_throw_icu_error(err);
            if(!bi)
                throw std::runtime_error("Failed to create break iterator");
            return bi;
        }

        template<typename CharType>
        index_type do_map(boundary_type t,
                          const CharType* begin,
                          const CharType* end,
                          const icu::Locale& loc,
                          const std::string& encoding)
        {
            std::unique_ptr<icu::BreakIterator> bi = get_iterator(t, loc);
            // Versions prior to ICU 55.2 returned wrong splits when used with UText input
#if BOOST_LOCALE_ICU_VERSION >= 5502
            UErrorCode err = U_ZERO_ERROR;
            BOOST_LOCALE_START_CONST_CONDITION
            if(sizeof(CharType) == 2 || util::is_char8_t<CharType>::value
               || (sizeof(CharType) == 1 && util::normalize_encoding(encoding) == "utf8"))
            {
                UText ut_stack = UTEXT_INITIALIZER;
                std::unique_ptr<UText> ut;
                if(sizeof(CharType) == 1)
                    ut.reset(utext_openUTF8(&ut_stack, reinterpret_cast<const char*>(begin), end - begin, &err));
                else {
                    static_assert(sizeof(UChar) == 2, "!");
                    ut.reset(utext_openUChars(&ut_stack, reinterpret_cast<const UChar*>(begin), end - begin, &err));
                }
                BOOST_LOCALE_END_CONST_CONDITION

                check_and_throw_icu_error(err);
                err = U_ZERO_ERROR;
                if(!ut)
                    throw std::runtime_error("Failed to create UText");
                bi->setText(ut.get(), err);
                check_and_throw_icu_error(err);
                return map_direct(t, bi.get(), end - begin);
            } else
#endif
            {
                icu_std_converter<CharType> cvt(encoding);
                const icu::UnicodeString str = cvt.icu(begin, end);
                bi->setText(str);
                const index_type indirect = map_direct(t, bi.get(), str.length());
                index_type indx = indirect;
                for(size_t i = 1; i < indirect.size(); i++) {
                    const size_t offset_indirect = indirect[i - 1].offset;
                    const size_t diff = indirect[i].offset - offset_indirect;
                    const size_t offset_direct = indx[i - 1].offset;
                    indx[i].offset = offset_direct + cvt.cut(str, begin, end, diff, offset_indirect, offset_direct);
                }
                return indx;
            }
        } // do_map

        template<typename CharType>
        class boundary_indexing_impl : public boundary_indexing<CharType> {
        public:
            boundary_indexing_impl(const cdata& data) : locale_(data.locale()), encoding_(data.encoding()) {}
            index_type map(boundary_type t, const CharType* begin, const CharType* end) const
            {
                return do_map<CharType>(t, begin, end, locale_, encoding_);
            }

        private:
            icu::Locale locale_;
            std::string encoding_;
        };

    }} // namespace boundary::impl_icu

    namespace impl_icu {
        std::locale create_boundary(const std::locale& in, const cdata& cd, char_facet_t type)
        {
            using namespace boost::locale::boundary::impl_icu;
            switch(type) {
                case char_facet_t::nochar: break;
                case char_facet_t::char_f: return std::locale(in, new boundary_indexing_impl<char>(cd));
                case char_facet_t::wchar_f: return std::locale(in, new boundary_indexing_impl<wchar_t>(cd));
#ifdef __cpp_char8_t
                case char_facet_t::char8_f: return std::locale(in, new boundary_indexing_impl<char8_t>(cd));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
                case char_facet_t::char16_f: return std::locale(in, new boundary_indexing_impl<char16_t>(cd));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
                case char_facet_t::char32_f: return std::locale(in, new boundary_indexing_impl<char32_t>(cd));
#endif
            }
            return in;
        }
    } // namespace impl_icu

}} // namespace boost::locale
