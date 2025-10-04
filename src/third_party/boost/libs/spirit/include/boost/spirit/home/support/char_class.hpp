/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman
    Copyright (c) 2001-2011 Hartmut Kaiser

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_CHAR_CLASS_NOVEMBER_10_2006_0907AM)
#define BOOST_SPIRIT_CHAR_CLASS_NOVEMBER_10_2006_0907AM

#if defined(_MSC_VER)
#pragma once
#endif

#include <boost/config.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/spirit/home/support/unused.hpp>
#include <boost/spirit/home/support/detail/is_spirit_tag.hpp>
#include <boost/type_traits/is_signed.hpp>
#include <boost/type_traits/make_unsigned.hpp>
#include <boost/type_traits/make_signed.hpp>

#if defined(BOOST_MSVC)
# pragma warning(push)
# pragma warning(disable: 4127) // conditional expression is constant
# pragma warning(disable: 4800) // 'int' : forcing value to bool 'true' or 'false' warning
#endif

namespace boost { namespace spirit { namespace detail
{
    // Here's the thing... typical encodings (except ASCII) deal with unsigned
    // integers > 127. ASCII uses only 127. Yet, most char and wchar_t are signed.
    // Thus, a char with value > 127 is negative (e.g. char 233 is -23). When you
    // cast this to an unsigned int with 32 bits, you get 4294967273!
    //
    // The trick is to cast to an unsigned version of the source char first
    // before casting to the target. {P.S. Don't worry about the code, the
    // optimizer will optimize the if-else branches}

    template <typename TargetChar, typename SourceChar>
    TargetChar cast_char(SourceChar ch)
    {
        if (is_signed<TargetChar>::value != is_signed<SourceChar>::value)
        {
            if (is_signed<SourceChar>::value)
            {
                 // source is signed, target is unsigned
                typedef typename make_unsigned<SourceChar>::type USourceChar;
                return TargetChar(USourceChar(ch));
            }
            else
            {
                 // source is unsigned, target is signed
                typedef typename make_signed<SourceChar>::type SSourceChar;
                return TargetChar(SSourceChar(ch));
            }
        }
        else
        {
            // source and target has same signedness
            return TargetChar(ch); // just cast
        }
    }
}}}

namespace boost { namespace spirit { namespace tag
{
    struct char_ { BOOST_SPIRIT_IS_TAG() };
    struct string { BOOST_SPIRIT_IS_TAG() };

    ///////////////////////////////////////////////////////////////////////////
    // classification tags
    struct alnum { BOOST_SPIRIT_IS_TAG() };
    struct alpha { BOOST_SPIRIT_IS_TAG() };
    struct digit { BOOST_SPIRIT_IS_TAG() };
    struct xdigit { BOOST_SPIRIT_IS_TAG() };
    struct cntrl { BOOST_SPIRIT_IS_TAG() };
    struct graph { BOOST_SPIRIT_IS_TAG() };
    struct print { BOOST_SPIRIT_IS_TAG() };
    struct punct { BOOST_SPIRIT_IS_TAG() };
    struct space { BOOST_SPIRIT_IS_TAG() };
    struct blank { BOOST_SPIRIT_IS_TAG() };

    ///////////////////////////////////////////////////////////////////////////
    // classification/conversion tags
    struct no_case { BOOST_SPIRIT_IS_TAG() };
    struct lower { BOOST_SPIRIT_IS_TAG() };
    struct upper { BOOST_SPIRIT_IS_TAG() };
    struct lowernum { BOOST_SPIRIT_IS_TAG() };
    struct uppernum { BOOST_SPIRIT_IS_TAG() };
    struct ucs4 { BOOST_SPIRIT_IS_TAG() };
    struct encoding { BOOST_SPIRIT_IS_TAG() };

#if defined(BOOST_SPIRIT_UNICODE)
///////////////////////////////////////////////////////////////////////////
//  Unicode Major Categories
///////////////////////////////////////////////////////////////////////////
    struct letter { BOOST_SPIRIT_IS_TAG() };
    struct mark { BOOST_SPIRIT_IS_TAG() };
    struct number { BOOST_SPIRIT_IS_TAG() };
    struct separator { BOOST_SPIRIT_IS_TAG() };
    struct other { BOOST_SPIRIT_IS_TAG() };
    struct punctuation { BOOST_SPIRIT_IS_TAG() };
    struct symbol { BOOST_SPIRIT_IS_TAG() };

///////////////////////////////////////////////////////////////////////////
//  Unicode General Categories
///////////////////////////////////////////////////////////////////////////
    struct uppercase_letter { BOOST_SPIRIT_IS_TAG() };
    struct lowercase_letter { BOOST_SPIRIT_IS_TAG() };
    struct titlecase_letter { BOOST_SPIRIT_IS_TAG() };
    struct modifier_letter { BOOST_SPIRIT_IS_TAG() };
    struct other_letter { BOOST_SPIRIT_IS_TAG() };

    struct nonspacing_mark { BOOST_SPIRIT_IS_TAG() };
    struct enclosing_mark { BOOST_SPIRIT_IS_TAG() };
    struct spacing_mark { BOOST_SPIRIT_IS_TAG() };

    struct decimal_number { BOOST_SPIRIT_IS_TAG() };
    struct letter_number { BOOST_SPIRIT_IS_TAG() };
    struct other_number { BOOST_SPIRIT_IS_TAG() };

    struct space_separator { BOOST_SPIRIT_IS_TAG() };
    struct line_separator { BOOST_SPIRIT_IS_TAG() };
    struct paragraph_separator { BOOST_SPIRIT_IS_TAG() };

    struct control { BOOST_SPIRIT_IS_TAG() };
    struct format { BOOST_SPIRIT_IS_TAG() };
    struct private_use { BOOST_SPIRIT_IS_TAG() };
    struct surrogate { BOOST_SPIRIT_IS_TAG() };
    struct unassigned { BOOST_SPIRIT_IS_TAG() };

    struct dash_punctuation { BOOST_SPIRIT_IS_TAG() };
    struct open_punctuation { BOOST_SPIRIT_IS_TAG() };
    struct close_punctuation { BOOST_SPIRIT_IS_TAG() };
    struct connector_punctuation { BOOST_SPIRIT_IS_TAG() };
    struct other_punctuation { BOOST_SPIRIT_IS_TAG() };
    struct initial_punctuation { BOOST_SPIRIT_IS_TAG() };
    struct final_punctuation { BOOST_SPIRIT_IS_TAG() };

    struct math_symbol { BOOST_SPIRIT_IS_TAG() };
    struct currency_symbol { BOOST_SPIRIT_IS_TAG() };
    struct modifier_symbol { BOOST_SPIRIT_IS_TAG() };
    struct other_symbol { BOOST_SPIRIT_IS_TAG() };

///////////////////////////////////////////////////////////////////////////
//  Unicode Derived Categories
///////////////////////////////////////////////////////////////////////////
    struct alphabetic { BOOST_SPIRIT_IS_TAG() };
    struct uppercase { BOOST_SPIRIT_IS_TAG() };
    struct lowercase { BOOST_SPIRIT_IS_TAG() };
    struct white_space { BOOST_SPIRIT_IS_TAG() };
    struct hex_digit { BOOST_SPIRIT_IS_TAG() };
    struct noncharacter_code_point { BOOST_SPIRIT_IS_TAG() };
    struct default_ignorable_code_point { BOOST_SPIRIT_IS_TAG() };

///////////////////////////////////////////////////////////////////////////
//  Unicode Scripts
///////////////////////////////////////////////////////////////////////////
    struct adlam { BOOST_SPIRIT_IS_TAG() };
    struct caucasian_albanian { BOOST_SPIRIT_IS_TAG() };
    struct ahom { BOOST_SPIRIT_IS_TAG() };
    struct arabic { BOOST_SPIRIT_IS_TAG() };
    struct imperial_aramaic { BOOST_SPIRIT_IS_TAG() };
    struct armenian { BOOST_SPIRIT_IS_TAG() };
    struct avestan { BOOST_SPIRIT_IS_TAG() };
    struct balinese { BOOST_SPIRIT_IS_TAG() };
    struct bamum { BOOST_SPIRIT_IS_TAG() };
    struct bassa_vah { BOOST_SPIRIT_IS_TAG() };
    struct batak { BOOST_SPIRIT_IS_TAG() };
    struct bengali { BOOST_SPIRIT_IS_TAG() };
    struct bhaiksuki { BOOST_SPIRIT_IS_TAG() };
    struct bopomofo { BOOST_SPIRIT_IS_TAG() };
    struct brahmi { BOOST_SPIRIT_IS_TAG() };
    struct braille { BOOST_SPIRIT_IS_TAG() };
    struct buginese { BOOST_SPIRIT_IS_TAG() };
    struct buhid { BOOST_SPIRIT_IS_TAG() };
    struct chakma { BOOST_SPIRIT_IS_TAG() };
    struct canadian_aboriginal { BOOST_SPIRIT_IS_TAG() };
    struct carian { BOOST_SPIRIT_IS_TAG() };
    struct cham { BOOST_SPIRIT_IS_TAG() };
    struct cherokee { BOOST_SPIRIT_IS_TAG() };
    struct chorasmian { BOOST_SPIRIT_IS_TAG() };
    struct coptic { BOOST_SPIRIT_IS_TAG() };
    struct cypro_minoan { BOOST_SPIRIT_IS_TAG() };
    struct cypriot { BOOST_SPIRIT_IS_TAG() };
    struct cyrillic { BOOST_SPIRIT_IS_TAG() };
    struct devanagari { BOOST_SPIRIT_IS_TAG() };
    struct dives_akuru { BOOST_SPIRIT_IS_TAG() };
    struct dogra { BOOST_SPIRIT_IS_TAG() };
    struct deseret { BOOST_SPIRIT_IS_TAG() };
    struct duployan { BOOST_SPIRIT_IS_TAG() };
    struct egyptian_hieroglyphs { BOOST_SPIRIT_IS_TAG() };
    struct elbasan { BOOST_SPIRIT_IS_TAG() };
    struct elymaic { BOOST_SPIRIT_IS_TAG() };
    struct ethiopic { BOOST_SPIRIT_IS_TAG() };
    struct georgian { BOOST_SPIRIT_IS_TAG() };
    struct glagolitic { BOOST_SPIRIT_IS_TAG() };
    struct gunjala_gondi { BOOST_SPIRIT_IS_TAG() };
    struct masaram_gondi { BOOST_SPIRIT_IS_TAG() };
    struct gothic { BOOST_SPIRIT_IS_TAG() };
    struct grantha { BOOST_SPIRIT_IS_TAG() };
    struct greek { BOOST_SPIRIT_IS_TAG() };
    struct gujarati { BOOST_SPIRIT_IS_TAG() };
    struct gurmukhi { BOOST_SPIRIT_IS_TAG() };
    struct hangul { BOOST_SPIRIT_IS_TAG() };
    struct han { BOOST_SPIRIT_IS_TAG() };
    struct hanunoo { BOOST_SPIRIT_IS_TAG() };
    struct hatran { BOOST_SPIRIT_IS_TAG() };
    struct hebrew { BOOST_SPIRIT_IS_TAG() };
    struct hiragana { BOOST_SPIRIT_IS_TAG() };
    struct anatolian_hieroglyphs { BOOST_SPIRIT_IS_TAG() };
    struct pahawh_hmong { BOOST_SPIRIT_IS_TAG() };
    struct nyiakeng_puachue_hmong { BOOST_SPIRIT_IS_TAG() };
    struct katakana_or_hiragana { BOOST_SPIRIT_IS_TAG() };
    struct old_hungarian { BOOST_SPIRIT_IS_TAG() };
    struct old_italic { BOOST_SPIRIT_IS_TAG() };
    struct javanese { BOOST_SPIRIT_IS_TAG() };
    struct kayah_li { BOOST_SPIRIT_IS_TAG() };
    struct katakana { BOOST_SPIRIT_IS_TAG() };
    struct kawi { BOOST_SPIRIT_IS_TAG() };
    struct kharoshthi { BOOST_SPIRIT_IS_TAG() };
    struct khmer { BOOST_SPIRIT_IS_TAG() };
    struct khojki { BOOST_SPIRIT_IS_TAG() };
    struct khitan_small_script { BOOST_SPIRIT_IS_TAG() };
    struct kannada { BOOST_SPIRIT_IS_TAG() };
    struct kaithi { BOOST_SPIRIT_IS_TAG() };
    struct tai_tham { BOOST_SPIRIT_IS_TAG() };
    struct lao { BOOST_SPIRIT_IS_TAG() };
    struct latin { BOOST_SPIRIT_IS_TAG() };
    struct lepcha { BOOST_SPIRIT_IS_TAG() };
    struct limbu { BOOST_SPIRIT_IS_TAG() };
    struct linear_a { BOOST_SPIRIT_IS_TAG() };
    struct linear_b { BOOST_SPIRIT_IS_TAG() };
    struct lisu { BOOST_SPIRIT_IS_TAG() };
    struct lycian { BOOST_SPIRIT_IS_TAG() };
    struct lydian { BOOST_SPIRIT_IS_TAG() };
    struct mahajani { BOOST_SPIRIT_IS_TAG() };
    struct makasar { BOOST_SPIRIT_IS_TAG() };
    struct mandaic { BOOST_SPIRIT_IS_TAG() };
    struct manichaean { BOOST_SPIRIT_IS_TAG() };
    struct marchen { BOOST_SPIRIT_IS_TAG() };
    struct medefaidrin { BOOST_SPIRIT_IS_TAG() };
    struct mende_kikakui { BOOST_SPIRIT_IS_TAG() };
    struct meroitic_cursive { BOOST_SPIRIT_IS_TAG() };
    struct meroitic_hieroglyphs { BOOST_SPIRIT_IS_TAG() };
    struct malayalam { BOOST_SPIRIT_IS_TAG() };
    struct modi { BOOST_SPIRIT_IS_TAG() };
    struct mongolian { BOOST_SPIRIT_IS_TAG() };
    struct mro { BOOST_SPIRIT_IS_TAG() };
    struct meetei_mayek { BOOST_SPIRIT_IS_TAG() };
    struct multani { BOOST_SPIRIT_IS_TAG() };
    struct myanmar { BOOST_SPIRIT_IS_TAG() };
    struct nag_mundari { BOOST_SPIRIT_IS_TAG() };
    struct nandinagari { BOOST_SPIRIT_IS_TAG() };
    struct old_north_arabian { BOOST_SPIRIT_IS_TAG() };
    struct nabataean { BOOST_SPIRIT_IS_TAG() };
    struct newa { BOOST_SPIRIT_IS_TAG() };
    struct nko { BOOST_SPIRIT_IS_TAG() };
    struct nushu { BOOST_SPIRIT_IS_TAG() };
    struct ogham { BOOST_SPIRIT_IS_TAG() };
    struct ol_chiki { BOOST_SPIRIT_IS_TAG() };
    struct old_turkic { BOOST_SPIRIT_IS_TAG() };
    struct oriya { BOOST_SPIRIT_IS_TAG() };
    struct osage { BOOST_SPIRIT_IS_TAG() };
    struct osmanya { BOOST_SPIRIT_IS_TAG() };
    struct old_uyghur { BOOST_SPIRIT_IS_TAG() };
    struct palmyrene { BOOST_SPIRIT_IS_TAG() };
    struct pau_cin_hau { BOOST_SPIRIT_IS_TAG() };
    struct old_permic { BOOST_SPIRIT_IS_TAG() };
    struct phags_pa { BOOST_SPIRIT_IS_TAG() };
    struct inscriptional_pahlavi { BOOST_SPIRIT_IS_TAG() };
    struct psalter_pahlavi { BOOST_SPIRIT_IS_TAG() };
    struct phoenician { BOOST_SPIRIT_IS_TAG() };
    struct miao { BOOST_SPIRIT_IS_TAG() };
    struct inscriptional_parthian { BOOST_SPIRIT_IS_TAG() };
    struct rejang { BOOST_SPIRIT_IS_TAG() };
    struct hanifi_rohingya { BOOST_SPIRIT_IS_TAG() };
    struct runic { BOOST_SPIRIT_IS_TAG() };
    struct samaritan { BOOST_SPIRIT_IS_TAG() };
    struct old_south_arabian { BOOST_SPIRIT_IS_TAG() };
    struct saurashtra { BOOST_SPIRIT_IS_TAG() };
    struct signwriting { BOOST_SPIRIT_IS_TAG() };
    struct shavian { BOOST_SPIRIT_IS_TAG() };
    struct sharada { BOOST_SPIRIT_IS_TAG() };
    struct siddham { BOOST_SPIRIT_IS_TAG() };
    struct khudawadi { BOOST_SPIRIT_IS_TAG() };
    struct sinhala { BOOST_SPIRIT_IS_TAG() };
    struct sogdian { BOOST_SPIRIT_IS_TAG() };
    struct old_sogdian { BOOST_SPIRIT_IS_TAG() };
    struct sora_sompeng { BOOST_SPIRIT_IS_TAG() };
    struct soyombo { BOOST_SPIRIT_IS_TAG() };
    struct sundanese { BOOST_SPIRIT_IS_TAG() };
    struct syloti_nagri { BOOST_SPIRIT_IS_TAG() };
    struct syriac { BOOST_SPIRIT_IS_TAG() };
    struct tagbanwa { BOOST_SPIRIT_IS_TAG() };
    struct takri { BOOST_SPIRIT_IS_TAG() };
    struct tai_le { BOOST_SPIRIT_IS_TAG() };
    struct new_tai_lue { BOOST_SPIRIT_IS_TAG() };
    struct tamil { BOOST_SPIRIT_IS_TAG() };
    struct tangut { BOOST_SPIRIT_IS_TAG() };
    struct tai_viet { BOOST_SPIRIT_IS_TAG() };
    struct telugu { BOOST_SPIRIT_IS_TAG() };
    struct tifinagh { BOOST_SPIRIT_IS_TAG() };
    struct tagalog { BOOST_SPIRIT_IS_TAG() };
    struct thaana { BOOST_SPIRIT_IS_TAG() };
    struct thai { BOOST_SPIRIT_IS_TAG() };
    struct tibetan { BOOST_SPIRIT_IS_TAG() };
    struct tirhuta { BOOST_SPIRIT_IS_TAG() };
    struct tangsa { BOOST_SPIRIT_IS_TAG() };
    struct toto { BOOST_SPIRIT_IS_TAG() };
    struct ugaritic { BOOST_SPIRIT_IS_TAG() };
    struct vai { BOOST_SPIRIT_IS_TAG() };
    struct vithkuqi { BOOST_SPIRIT_IS_TAG() };
    struct warang_citi { BOOST_SPIRIT_IS_TAG() };
    struct wancho { BOOST_SPIRIT_IS_TAG() };
    struct old_persian { BOOST_SPIRIT_IS_TAG() };
    struct cuneiform { BOOST_SPIRIT_IS_TAG() };
    struct yezidi { BOOST_SPIRIT_IS_TAG() };
    struct yi { BOOST_SPIRIT_IS_TAG() };
    struct zanabazar_square { BOOST_SPIRIT_IS_TAG() };
    struct inherited { BOOST_SPIRIT_IS_TAG() };
    struct common { BOOST_SPIRIT_IS_TAG() };
    struct unknown { BOOST_SPIRIT_IS_TAG() };
#endif

    ///////////////////////////////////////////////////////////////////////////
    // This composite tag type encodes both the character
    // set and the specific char tag (used for classification
    // or conversion). char_code_base and char_encoding_base
    // can be used to test for modifier membership (see modifier.hpp)
    template <typename CharClass>
    struct char_code_base {};

    template <typename CharEncoding>
    struct char_encoding_base {};

    template <typename CharClass, typename CharEncoding>
    struct char_code
        : char_code_base<CharClass>, char_encoding_base<CharEncoding>
    {
        BOOST_SPIRIT_IS_TAG()
        typedef CharEncoding char_encoding; // e.g. ascii
        typedef CharClass char_class;       // e.g. tag::alnum
    };
}}}

namespace boost { namespace spirit { namespace char_class
{
    ///////////////////////////////////////////////////////////////////////////
    // Test characters for classification
    template <typename CharEncoding>
    struct classify
    {
        typedef typename CharEncoding::classify_type char_type;

#define BOOST_SPIRIT_CLASSIFY(name, isname)                                     \
        template <typename Char>                                                \
        static bool                                                             \
        is(tag::name, Char ch)                                                  \
        {                                                                       \
            return CharEncoding::isname                                         \
                BOOST_PREVENT_MACRO_SUBSTITUTION                                \
                    (detail::cast_char<char_type>(ch));                         \
        }                                                                       \
        /***/

        BOOST_SPIRIT_CLASSIFY(char_, ischar)
        BOOST_SPIRIT_CLASSIFY(alnum, isalnum)
        BOOST_SPIRIT_CLASSIFY(alpha, isalpha)
        BOOST_SPIRIT_CLASSIFY(digit, isdigit)
        BOOST_SPIRIT_CLASSIFY(xdigit, isxdigit)
        BOOST_SPIRIT_CLASSIFY(cntrl, iscntrl)
        BOOST_SPIRIT_CLASSIFY(graph, isgraph)
        BOOST_SPIRIT_CLASSIFY(lower, islower)
        BOOST_SPIRIT_CLASSIFY(print, isprint)
        BOOST_SPIRIT_CLASSIFY(punct, ispunct)
        BOOST_SPIRIT_CLASSIFY(space, isspace)
        BOOST_SPIRIT_CLASSIFY(blank, isblank)
        BOOST_SPIRIT_CLASSIFY(upper, isupper)

#undef BOOST_SPIRIT_CLASSIFY

        template <typename Char>
        static bool
        is(tag::lowernum, Char ch)
        {
            return CharEncoding::islower(detail::cast_char<char_type>(ch)) ||
                   CharEncoding::isdigit(detail::cast_char<char_type>(ch));
        }

        template <typename Char>
        static bool
        is(tag::uppernum, Char ch)
        {
            return CharEncoding::isupper(detail::cast_char<char_type>(ch)) ||
                   CharEncoding::isdigit(detail::cast_char<char_type>(ch));
        }

#if defined(BOOST_SPIRIT_UNICODE)

#define BOOST_SPIRIT_UNICODE_CLASSIFY(name)                                     \
        template <typename Char>                                                \
        static bool                                                             \
        is(tag::name, Char ch)                                                  \
        {                                                                       \
            return CharEncoding::is_##name(detail::cast_char<char_type>(ch));   \
        }                                                                       \
        /***/

///////////////////////////////////////////////////////////////////////////
//  Unicode Major Categories
///////////////////////////////////////////////////////////////////////////
    BOOST_SPIRIT_UNICODE_CLASSIFY(letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY(mark)
    BOOST_SPIRIT_UNICODE_CLASSIFY(number)
    BOOST_SPIRIT_UNICODE_CLASSIFY(separator)
    BOOST_SPIRIT_UNICODE_CLASSIFY(other)
    BOOST_SPIRIT_UNICODE_CLASSIFY(punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY(symbol)

///////////////////////////////////////////////////////////////////////////
//  Unicode General Categories
///////////////////////////////////////////////////////////////////////////
    BOOST_SPIRIT_UNICODE_CLASSIFY(uppercase_letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY(lowercase_letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY(titlecase_letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY(modifier_letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY(other_letter)

    BOOST_SPIRIT_UNICODE_CLASSIFY(nonspacing_mark)
    BOOST_SPIRIT_UNICODE_CLASSIFY(enclosing_mark)
    BOOST_SPIRIT_UNICODE_CLASSIFY(spacing_mark)

    BOOST_SPIRIT_UNICODE_CLASSIFY(decimal_number)
    BOOST_SPIRIT_UNICODE_CLASSIFY(letter_number)
    BOOST_SPIRIT_UNICODE_CLASSIFY(other_number)

    BOOST_SPIRIT_UNICODE_CLASSIFY(space_separator)
    BOOST_SPIRIT_UNICODE_CLASSIFY(line_separator)
    BOOST_SPIRIT_UNICODE_CLASSIFY(paragraph_separator)

    BOOST_SPIRIT_UNICODE_CLASSIFY(control)
    BOOST_SPIRIT_UNICODE_CLASSIFY(format)
    BOOST_SPIRIT_UNICODE_CLASSIFY(private_use)
    BOOST_SPIRIT_UNICODE_CLASSIFY(surrogate)
    BOOST_SPIRIT_UNICODE_CLASSIFY(unassigned)

    BOOST_SPIRIT_UNICODE_CLASSIFY(dash_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY(open_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY(close_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY(connector_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY(other_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY(initial_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY(final_punctuation)

    BOOST_SPIRIT_UNICODE_CLASSIFY(math_symbol)
    BOOST_SPIRIT_UNICODE_CLASSIFY(currency_symbol)
    BOOST_SPIRIT_UNICODE_CLASSIFY(modifier_symbol)
    BOOST_SPIRIT_UNICODE_CLASSIFY(other_symbol)

///////////////////////////////////////////////////////////////////////////
//  Unicode Derived Categories
///////////////////////////////////////////////////////////////////////////
    BOOST_SPIRIT_UNICODE_CLASSIFY(alphabetic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(uppercase)
    BOOST_SPIRIT_UNICODE_CLASSIFY(lowercase)
    BOOST_SPIRIT_UNICODE_CLASSIFY(white_space)
    BOOST_SPIRIT_UNICODE_CLASSIFY(hex_digit)
    BOOST_SPIRIT_UNICODE_CLASSIFY(noncharacter_code_point)
    BOOST_SPIRIT_UNICODE_CLASSIFY(default_ignorable_code_point)

///////////////////////////////////////////////////////////////////////////
//  Unicode Scripts
///////////////////////////////////////////////////////////////////////////
    BOOST_SPIRIT_UNICODE_CLASSIFY(adlam)
    BOOST_SPIRIT_UNICODE_CLASSIFY(caucasian_albanian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(ahom)
    BOOST_SPIRIT_UNICODE_CLASSIFY(arabic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(imperial_aramaic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(armenian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(avestan)
    BOOST_SPIRIT_UNICODE_CLASSIFY(balinese)
    BOOST_SPIRIT_UNICODE_CLASSIFY(bamum)
    BOOST_SPIRIT_UNICODE_CLASSIFY(bassa_vah)
    BOOST_SPIRIT_UNICODE_CLASSIFY(batak)
    BOOST_SPIRIT_UNICODE_CLASSIFY(bengali)
    BOOST_SPIRIT_UNICODE_CLASSIFY(bhaiksuki)
    BOOST_SPIRIT_UNICODE_CLASSIFY(bopomofo)
    BOOST_SPIRIT_UNICODE_CLASSIFY(brahmi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(braille)
    BOOST_SPIRIT_UNICODE_CLASSIFY(buginese)
    BOOST_SPIRIT_UNICODE_CLASSIFY(buhid)
    BOOST_SPIRIT_UNICODE_CLASSIFY(chakma)
    BOOST_SPIRIT_UNICODE_CLASSIFY(canadian_aboriginal)
    BOOST_SPIRIT_UNICODE_CLASSIFY(carian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(cham)
    BOOST_SPIRIT_UNICODE_CLASSIFY(cherokee)
    BOOST_SPIRIT_UNICODE_CLASSIFY(chorasmian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(coptic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(cypro_minoan)
    BOOST_SPIRIT_UNICODE_CLASSIFY(cypriot)
    BOOST_SPIRIT_UNICODE_CLASSIFY(cyrillic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(devanagari)
    BOOST_SPIRIT_UNICODE_CLASSIFY(dives_akuru)
    BOOST_SPIRIT_UNICODE_CLASSIFY(dogra)
    BOOST_SPIRIT_UNICODE_CLASSIFY(deseret)
    BOOST_SPIRIT_UNICODE_CLASSIFY(duployan)
    BOOST_SPIRIT_UNICODE_CLASSIFY(egyptian_hieroglyphs)
    BOOST_SPIRIT_UNICODE_CLASSIFY(elbasan)
    BOOST_SPIRIT_UNICODE_CLASSIFY(elymaic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(ethiopic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(georgian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(glagolitic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(gunjala_gondi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(masaram_gondi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(gothic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(grantha)
    BOOST_SPIRIT_UNICODE_CLASSIFY(greek)
    BOOST_SPIRIT_UNICODE_CLASSIFY(gujarati)
    BOOST_SPIRIT_UNICODE_CLASSIFY(gurmukhi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(hangul)
    BOOST_SPIRIT_UNICODE_CLASSIFY(han)
    BOOST_SPIRIT_UNICODE_CLASSIFY(hanunoo)
    BOOST_SPIRIT_UNICODE_CLASSIFY(hatran)
    BOOST_SPIRIT_UNICODE_CLASSIFY(hebrew)
    BOOST_SPIRIT_UNICODE_CLASSIFY(hiragana)
    BOOST_SPIRIT_UNICODE_CLASSIFY(anatolian_hieroglyphs)
    BOOST_SPIRIT_UNICODE_CLASSIFY(pahawh_hmong)
    BOOST_SPIRIT_UNICODE_CLASSIFY(nyiakeng_puachue_hmong)
    BOOST_SPIRIT_UNICODE_CLASSIFY(katakana_or_hiragana)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_hungarian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_italic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(javanese)
    BOOST_SPIRIT_UNICODE_CLASSIFY(kayah_li)
    BOOST_SPIRIT_UNICODE_CLASSIFY(katakana)
    BOOST_SPIRIT_UNICODE_CLASSIFY(kawi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(kharoshthi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(khmer)
    BOOST_SPIRIT_UNICODE_CLASSIFY(khojki)
    BOOST_SPIRIT_UNICODE_CLASSIFY(khitan_small_script)
    BOOST_SPIRIT_UNICODE_CLASSIFY(kannada)
    BOOST_SPIRIT_UNICODE_CLASSIFY(kaithi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tai_tham)
    BOOST_SPIRIT_UNICODE_CLASSIFY(lao)
    BOOST_SPIRIT_UNICODE_CLASSIFY(latin)
    BOOST_SPIRIT_UNICODE_CLASSIFY(lepcha)
    BOOST_SPIRIT_UNICODE_CLASSIFY(limbu)
    BOOST_SPIRIT_UNICODE_CLASSIFY(linear_a)
    BOOST_SPIRIT_UNICODE_CLASSIFY(linear_b)
    BOOST_SPIRIT_UNICODE_CLASSIFY(lisu)
    BOOST_SPIRIT_UNICODE_CLASSIFY(lycian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(lydian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(mahajani)
    BOOST_SPIRIT_UNICODE_CLASSIFY(makasar)
    BOOST_SPIRIT_UNICODE_CLASSIFY(mandaic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(manichaean)
    BOOST_SPIRIT_UNICODE_CLASSIFY(marchen)
    BOOST_SPIRIT_UNICODE_CLASSIFY(medefaidrin)
    BOOST_SPIRIT_UNICODE_CLASSIFY(mende_kikakui)
    BOOST_SPIRIT_UNICODE_CLASSIFY(meroitic_cursive)
    BOOST_SPIRIT_UNICODE_CLASSIFY(meroitic_hieroglyphs)
    BOOST_SPIRIT_UNICODE_CLASSIFY(malayalam)
    BOOST_SPIRIT_UNICODE_CLASSIFY(modi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(mongolian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(mro)
    BOOST_SPIRIT_UNICODE_CLASSIFY(meetei_mayek)
    BOOST_SPIRIT_UNICODE_CLASSIFY(multani)
    BOOST_SPIRIT_UNICODE_CLASSIFY(myanmar)
    BOOST_SPIRIT_UNICODE_CLASSIFY(nag_mundari)
    BOOST_SPIRIT_UNICODE_CLASSIFY(nandinagari)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_north_arabian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(nabataean)
    BOOST_SPIRIT_UNICODE_CLASSIFY(newa)
    BOOST_SPIRIT_UNICODE_CLASSIFY(nko)
    BOOST_SPIRIT_UNICODE_CLASSIFY(nushu)
    BOOST_SPIRIT_UNICODE_CLASSIFY(ogham)
    BOOST_SPIRIT_UNICODE_CLASSIFY(ol_chiki)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_turkic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(oriya)
    BOOST_SPIRIT_UNICODE_CLASSIFY(osage)
    BOOST_SPIRIT_UNICODE_CLASSIFY(osmanya)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_uyghur)
    BOOST_SPIRIT_UNICODE_CLASSIFY(palmyrene)
    BOOST_SPIRIT_UNICODE_CLASSIFY(pau_cin_hau)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_permic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(phags_pa)
    BOOST_SPIRIT_UNICODE_CLASSIFY(inscriptional_pahlavi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(psalter_pahlavi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(phoenician)
    BOOST_SPIRIT_UNICODE_CLASSIFY(miao)
    BOOST_SPIRIT_UNICODE_CLASSIFY(inscriptional_parthian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(rejang)
    BOOST_SPIRIT_UNICODE_CLASSIFY(hanifi_rohingya)
    BOOST_SPIRIT_UNICODE_CLASSIFY(runic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(samaritan)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_south_arabian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(saurashtra)
    BOOST_SPIRIT_UNICODE_CLASSIFY(signwriting)
    BOOST_SPIRIT_UNICODE_CLASSIFY(shavian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(sharada)
    BOOST_SPIRIT_UNICODE_CLASSIFY(siddham)
    BOOST_SPIRIT_UNICODE_CLASSIFY(khudawadi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(sinhala)
    BOOST_SPIRIT_UNICODE_CLASSIFY(sogdian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_sogdian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(sora_sompeng)
    BOOST_SPIRIT_UNICODE_CLASSIFY(soyombo)
    BOOST_SPIRIT_UNICODE_CLASSIFY(sundanese)
    BOOST_SPIRIT_UNICODE_CLASSIFY(syloti_nagri)
    BOOST_SPIRIT_UNICODE_CLASSIFY(syriac)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tagbanwa)
    BOOST_SPIRIT_UNICODE_CLASSIFY(takri)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tai_le)
    BOOST_SPIRIT_UNICODE_CLASSIFY(new_tai_lue)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tamil)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tangut)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tai_viet)
    BOOST_SPIRIT_UNICODE_CLASSIFY(telugu)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tifinagh)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tagalog)
    BOOST_SPIRIT_UNICODE_CLASSIFY(thaana)
    BOOST_SPIRIT_UNICODE_CLASSIFY(thai)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tibetan)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tirhuta)
    BOOST_SPIRIT_UNICODE_CLASSIFY(tangsa)
    BOOST_SPIRIT_UNICODE_CLASSIFY(toto)
    BOOST_SPIRIT_UNICODE_CLASSIFY(ugaritic)
    BOOST_SPIRIT_UNICODE_CLASSIFY(vai)
    BOOST_SPIRIT_UNICODE_CLASSIFY(vithkuqi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(warang_citi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(wancho)
    BOOST_SPIRIT_UNICODE_CLASSIFY(old_persian)
    BOOST_SPIRIT_UNICODE_CLASSIFY(cuneiform)
    BOOST_SPIRIT_UNICODE_CLASSIFY(yezidi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(yi)
    BOOST_SPIRIT_UNICODE_CLASSIFY(zanabazar_square)
    BOOST_SPIRIT_UNICODE_CLASSIFY(inherited)
    BOOST_SPIRIT_UNICODE_CLASSIFY(common)
    BOOST_SPIRIT_UNICODE_CLASSIFY(unknown)

#undef BOOST_SPIRIT_UNICODE_CLASSIFY
#endif

    };

    ///////////////////////////////////////////////////////////////////////////
    // Convert characters
    template <typename CharEncoding>
    struct convert
    {
        typedef typename CharEncoding::classify_type char_type;

        template <typename Char>
        static Char
        to(tag::lower, Char ch)
        {
            return static_cast<Char>(
                CharEncoding::tolower(detail::cast_char<char_type>(ch)));
        }

        template <typename Char>
        static Char
        to(tag::upper, Char ch)
        {
            return static_cast<Char>(
                CharEncoding::toupper(detail::cast_char<char_type>(ch)));
        }

        template <typename Char>
        static Char
        to(tag::ucs4, Char ch)
        {
            return static_cast<Char>(
                CharEncoding::toucs4(detail::cast_char<char_type>(ch)));
        }

        template <typename Char>
        static Char
        to(unused_type, Char ch)
        {
            return ch;
        }
    };

    ///////////////////////////////////////////////////////////////////////////
    // Info on character classification
    template <typename CharEncoding>
    struct what
    {
#define BOOST_SPIRIT_CLASSIFY_WHAT(name, isname)                                \
        static char const* is(tag::name)                                        \
        {                                                                       \
            return isname;                                                      \
        }                                                                       \
        /***/

        BOOST_SPIRIT_CLASSIFY_WHAT(char_, "char")
        BOOST_SPIRIT_CLASSIFY_WHAT(alnum, "alnum")
        BOOST_SPIRIT_CLASSIFY_WHAT(alpha, "alpha")
        BOOST_SPIRIT_CLASSIFY_WHAT(digit, "digit")
        BOOST_SPIRIT_CLASSIFY_WHAT(xdigit, "xdigit")
        BOOST_SPIRIT_CLASSIFY_WHAT(cntrl, "cntrl")
        BOOST_SPIRIT_CLASSIFY_WHAT(graph, "graph")
        BOOST_SPIRIT_CLASSIFY_WHAT(lower, "lower")
        BOOST_SPIRIT_CLASSIFY_WHAT(lowernum, "lowernum")
        BOOST_SPIRIT_CLASSIFY_WHAT(print, "print")
        BOOST_SPIRIT_CLASSIFY_WHAT(punct, "punct")
        BOOST_SPIRIT_CLASSIFY_WHAT(space, "space")
        BOOST_SPIRIT_CLASSIFY_WHAT(blank, "blank")
        BOOST_SPIRIT_CLASSIFY_WHAT(upper, "upper")
        BOOST_SPIRIT_CLASSIFY_WHAT(uppernum, "uppernum")
        BOOST_SPIRIT_CLASSIFY_WHAT(ucs4, "ucs4")

#undef BOOST_SPIRIT_CLASSIFY_WHAT

#if defined(BOOST_SPIRIT_UNICODE)

#define BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(name)                                \
        static char const* is(tag::name)                                        \
        {                                                                       \
            return BOOST_PP_STRINGIZE(name);                                    \
        }                                                                       \
        /***/

///////////////////////////////////////////////////////////////////////////
//  Unicode Major Categories
///////////////////////////////////////////////////////////////////////////
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(mark)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(number)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(separator)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(other)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(symbol)

///////////////////////////////////////////////////////////////////////////
//  Unicode General Categories
///////////////////////////////////////////////////////////////////////////
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(uppercase_letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(lowercase_letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(titlecase_letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(modifier_letter)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(other_letter)

    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(nonspacing_mark)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(enclosing_mark)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(spacing_mark)

    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(decimal_number)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(letter_number)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(other_number)

    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(space_separator)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(line_separator)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(paragraph_separator)

    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(control)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(format)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(private_use)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(surrogate)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(unassigned)

    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(dash_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(open_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(close_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(connector_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(other_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(initial_punctuation)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(final_punctuation)

    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(math_symbol)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(currency_symbol)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(modifier_symbol)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(other_symbol)

///////////////////////////////////////////////////////////////////////////
//  Unicode Derived Categories
///////////////////////////////////////////////////////////////////////////
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(alphabetic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(uppercase)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(lowercase)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(white_space)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(hex_digit)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(noncharacter_code_point)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(default_ignorable_code_point)

///////////////////////////////////////////////////////////////////////////
//  Unicode Scripts
///////////////////////////////////////////////////////////////////////////
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(adlam)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(caucasian_albanian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(ahom)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(arabic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(imperial_aramaic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(armenian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(avestan)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(balinese)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(bamum)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(bassa_vah)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(batak)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(bengali)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(bhaiksuki)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(bopomofo)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(brahmi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(braille)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(buginese)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(buhid)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(chakma)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(canadian_aboriginal)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(carian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(cham)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(cherokee)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(chorasmian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(coptic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(cypro_minoan)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(cypriot)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(cyrillic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(devanagari)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(dives_akuru)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(dogra)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(deseret)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(duployan)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(egyptian_hieroglyphs)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(elbasan)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(elymaic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(ethiopic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(georgian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(glagolitic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(gunjala_gondi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(masaram_gondi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(gothic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(grantha)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(greek)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(gujarati)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(gurmukhi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(hangul)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(han)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(hanunoo)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(hatran)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(hebrew)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(hiragana)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(anatolian_hieroglyphs)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(pahawh_hmong)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(nyiakeng_puachue_hmong)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(katakana_or_hiragana)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_hungarian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_italic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(javanese)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(kayah_li)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(katakana)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(kawi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(kharoshthi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(khmer)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(khojki)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(khitan_small_script)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(kannada)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(kaithi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tai_tham)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(lao)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(latin)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(lepcha)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(limbu)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(linear_a)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(linear_b)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(lisu)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(lycian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(lydian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(mahajani)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(makasar)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(mandaic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(manichaean)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(marchen)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(medefaidrin)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(mende_kikakui)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(meroitic_cursive)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(meroitic_hieroglyphs)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(malayalam)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(modi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(mongolian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(mro)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(meetei_mayek)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(multani)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(myanmar)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(nag_mundari)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(nandinagari)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_north_arabian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(nabataean)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(newa)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(nko)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(nushu)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(ogham)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(ol_chiki)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_turkic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(oriya)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(osage)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(osmanya)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_uyghur)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(palmyrene)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(pau_cin_hau)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_permic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(phags_pa)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(inscriptional_pahlavi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(psalter_pahlavi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(phoenician)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(miao)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(inscriptional_parthian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(rejang)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(hanifi_rohingya)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(runic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(samaritan)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_south_arabian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(saurashtra)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(signwriting)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(shavian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(sharada)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(siddham)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(khudawadi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(sinhala)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(sogdian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_sogdian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(sora_sompeng)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(soyombo)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(sundanese)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(syloti_nagri)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(syriac)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tagbanwa)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(takri)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tai_le)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(new_tai_lue)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tamil)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tangut)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tai_viet)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(telugu)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tifinagh)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tagalog)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(thaana)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(thai)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tibetan)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tirhuta)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(tangsa)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(toto)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(ugaritic)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(vai)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(vithkuqi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(warang_citi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(wancho)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(old_persian)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(cuneiform)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(yezidi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(yi)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(zanabazar_square)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(inherited)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(common)
    BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT(unknown)

#undef BOOST_SPIRIT_UNICODE_CLASSIFY_WHAT
#endif

    };
}}}

namespace boost { namespace spirit { namespace traits
{
    ///////////////////////////////////////////////////////////////////////////
    template <typename CharParam, typename CharEncoding>
    struct ischar
    {
        static bool call(CharParam const& ch)
        {
           return CharEncoding::ischar(int(ch));
        }
    };
}}}

#if defined(BOOST_MSVC)
# pragma warning(pop)
#endif

#endif


