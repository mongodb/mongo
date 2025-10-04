/*=============================================================================
    Copyright (c) 2001-2014 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(BOOST_SPIRIT_X3_UNICODE_JAN_20_2012_1218AM)
#define BOOST_SPIRIT_X3_UNICODE_JAN_20_2012_1218AM

#include <boost/spirit/home/x3/char/char_parser.hpp>
#include <boost/spirit/home/x3/char/char.hpp>
#include <boost/spirit/home/x3/char/detail/cast_char.hpp>
#include <boost/spirit/home/support/char_encoding/unicode.hpp>

namespace boost { namespace spirit { namespace x3
{
    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Major Categories
    ///////////////////////////////////////////////////////////////////////////
    struct char_tag;
    struct alnum_tag;
    struct alpha_tag;
    struct blank_tag;
    struct cntrl_tag;
    struct digit_tag;
    struct graph_tag;
    struct print_tag;
    struct punct_tag;
    struct space_tag;
    struct xdigit_tag;
    struct lower_tag;
    struct upper_tag;

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Major Categories
    ///////////////////////////////////////////////////////////////////////////
    struct letter_tag {};
    struct mark_tag {};
    struct number_tag {};
    struct separator_tag {};
    struct other_tag {};
    struct punctuation_tag {};
    struct symbol_tag {};

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode General Categories
    ///////////////////////////////////////////////////////////////////////////
    struct uppercase_letter_tag {};
    struct lowercase_letter_tag {};
    struct titlecase_letter_tag {};
    struct modifier_letter_tag {};
    struct other_letter_tag {};

    struct nonspacing_mark_tag {};
    struct enclosing_mark_tag {};
    struct spacing_mark_tag {};

    struct decimal_number_tag {};
    struct letter_number_tag {};
    struct other_number_tag {};

    struct space_separator_tag {};
    struct line_separator_tag {};
    struct paragraph_separator_tag {};

    struct control_tag {};
    struct format_tag {};
    struct private_use_tag {};
    struct surrogate_tag {};
    struct unassigned_tag {};

    struct dash_punctuation_tag {};
    struct open_punctuation_tag {};
    struct close_punctuation_tag {};
    struct connector_punctuation_tag {};
    struct other_punctuation_tag {};
    struct initial_punctuation_tag {};
    struct final_punctuation_tag {};

    struct math_symbol_tag {};
    struct currency_symbol_tag {};
    struct modifier_symbol_tag {};
    struct other_symbol_tag {};

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Derived Categories
    ///////////////////////////////////////////////////////////////////////////
    struct alphabetic_tag {};
    struct uppercase_tag {};
    struct lowercase_tag {};
    struct white_space_tag {};
    struct hex_digit_tag {};
    struct noncharacter_code_point_tag {};
    struct default_ignorable_code_point_tag {};

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Scripts
    ///////////////////////////////////////////////////////////////////////////
    struct adlam_tag {};
    struct caucasian_albanian_tag {};
    struct ahom_tag {};
    struct arabic_tag {};
    struct imperial_aramaic_tag {};
    struct armenian_tag {};
    struct avestan_tag {};
    struct balinese_tag {};
    struct bamum_tag {};
    struct bassa_vah_tag {};
    struct batak_tag {};
    struct bengali_tag {};
    struct bhaiksuki_tag {};
    struct bopomofo_tag {};
    struct brahmi_tag {};
    struct braille_tag {};
    struct buginese_tag {};
    struct buhid_tag {};
    struct chakma_tag {};
    struct canadian_aboriginal_tag {};
    struct carian_tag {};
    struct cham_tag {};
    struct cherokee_tag {};
    struct chorasmian_tag {};
    struct coptic_tag {};
    struct cypro_minoan_tag {};
    struct cypriot_tag {};
    struct cyrillic_tag {};
    struct devanagari_tag {};
    struct dives_akuru_tag {};
    struct dogra_tag {};
    struct deseret_tag {};
    struct duployan_tag {};
    struct egyptian_hieroglyphs_tag {};
    struct elbasan_tag {};
    struct elymaic_tag {};
    struct ethiopic_tag {};
    struct georgian_tag {};
    struct glagolitic_tag {};
    struct gunjala_gondi_tag {};
    struct masaram_gondi_tag {};
    struct gothic_tag {};
    struct grantha_tag {};
    struct greek_tag {};
    struct gujarati_tag {};
    struct gurmukhi_tag {};
    struct hangul_tag {};
    struct han_tag {};
    struct hanunoo_tag {};
    struct hatran_tag {};
    struct hebrew_tag {};
    struct hiragana_tag {};
    struct anatolian_hieroglyphs_tag {};
    struct pahawh_hmong_tag {};
    struct nyiakeng_puachue_hmong_tag {};
    struct katakana_or_hiragana_tag {};
    struct old_hungarian_tag {};
    struct old_italic_tag {};
    struct javanese_tag {};
    struct kayah_li_tag {};
    struct katakana_tag {};
    struct kawi_tag {};
    struct kharoshthi_tag {};
    struct khmer_tag {};
    struct khojki_tag {};
    struct khitan_small_script_tag {};
    struct kannada_tag {};
    struct kaithi_tag {};
    struct tai_tham_tag {};
    struct lao_tag {};
    struct latin_tag {};
    struct lepcha_tag {};
    struct limbu_tag {};
    struct linear_a_tag {};
    struct linear_b_tag {};
    struct lisu_tag {};
    struct lycian_tag {};
    struct lydian_tag {};
    struct mahajani_tag {};
    struct makasar_tag {};
    struct mandaic_tag {};
    struct manichaean_tag {};
    struct marchen_tag {};
    struct medefaidrin_tag {};
    struct mende_kikakui_tag {};
    struct meroitic_cursive_tag {};
    struct meroitic_hieroglyphs_tag {};
    struct malayalam_tag {};
    struct modi_tag {};
    struct mongolian_tag {};
    struct mro_tag {};
    struct meetei_mayek_tag {};
    struct multani_tag {};
    struct myanmar_tag {};
    struct nag_mundari_tag {};
    struct nandinagari_tag {};
    struct old_north_arabian_tag {};
    struct nabataean_tag {};
    struct newa_tag {};
    struct nko_tag {};
    struct nushu_tag {};
    struct ogham_tag {};
    struct ol_chiki_tag {};
    struct old_turkic_tag {};
    struct oriya_tag {};
    struct osage_tag {};
    struct osmanya_tag {};
    struct old_uyghur_tag {};
    struct palmyrene_tag {};
    struct pau_cin_hau_tag {};
    struct old_permic_tag {};
    struct phags_pa_tag {};
    struct inscriptional_pahlavi_tag {};
    struct psalter_pahlavi_tag {};
    struct phoenician_tag {};
    struct miao_tag {};
    struct inscriptional_parthian_tag {};
    struct rejang_tag {};
    struct hanifi_rohingya_tag {};
    struct runic_tag {};
    struct samaritan_tag {};
    struct old_south_arabian_tag {};
    struct saurashtra_tag {};
    struct signwriting_tag {};
    struct shavian_tag {};
    struct sharada_tag {};
    struct siddham_tag {};
    struct khudawadi_tag {};
    struct sinhala_tag {};
    struct sogdian_tag {};
    struct old_sogdian_tag {};
    struct sora_sompeng_tag {};
    struct soyombo_tag {};
    struct sundanese_tag {};
    struct syloti_nagri_tag {};
    struct syriac_tag {};
    struct tagbanwa_tag {};
    struct takri_tag {};
    struct tai_le_tag {};
    struct new_tai_lue_tag {};
    struct tamil_tag {};
    struct tangut_tag {};
    struct tai_viet_tag {};
    struct telugu_tag {};
    struct tifinagh_tag {};
    struct tagalog_tag {};
    struct thaana_tag {};
    struct thai_tag {};
    struct tibetan_tag {};
    struct tirhuta_tag {};
    struct tangsa_tag {};
    struct toto_tag {};
    struct ugaritic_tag {};
    struct vai_tag {};
    struct vithkuqi_tag {};
    struct warang_citi_tag {};
    struct wancho_tag {};
    struct old_persian_tag {};
    struct cuneiform_tag {};
    struct yezidi_tag {};
    struct yi_tag {};
    struct zanabazar_square_tag {};
    struct inherited_tag {};
    struct common_tag {};
    struct unknown_tag {};

    ///////////////////////////////////////////////////////////////////////////
    struct unicode_char_class_base
    {
        typedef char_encoding::unicode encoding;
        typedef char_encoding::unicode::char_type char_type;

#define BOOST_SPIRIT_X3_BASIC_CLASSIFY(name)                                     \
        template <typename Char>                                                 \
        static bool                                                              \
        is(name##_tag, Char ch)                                                  \
        {                                                                        \
            return encoding::is ##name                                           \
                BOOST_PREVENT_MACRO_SUBSTITUTION                                 \
                    (detail::cast_char<char_type>(ch));                          \
        }                                                                        \
        /***/

#define BOOST_SPIRIT_X3_CLASSIFY(name)                                           \
        template <typename Char>                                                 \
        static bool                                                              \
        is(name##_tag, Char ch)                                                  \
        {                                                                        \
            return encoding::is_##name                                           \
                BOOST_PREVENT_MACRO_SUBSTITUTION                                 \
                    (detail::cast_char<char_type>(ch));                          \
        }                                                                        \
        /***/


    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Major Categories
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(char)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(alnum)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(alpha)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(digit)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(xdigit)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(cntrl)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(graph)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(lower)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(print)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(punct)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(space)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(blank)
        BOOST_SPIRIT_X3_BASIC_CLASSIFY(upper)

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Major Categories
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CLASSIFY(letter)
        BOOST_SPIRIT_X3_CLASSIFY(mark)
        BOOST_SPIRIT_X3_CLASSIFY(number)
        BOOST_SPIRIT_X3_CLASSIFY(separator)
        BOOST_SPIRIT_X3_CLASSIFY(other)
        BOOST_SPIRIT_X3_CLASSIFY(punctuation)
        BOOST_SPIRIT_X3_CLASSIFY(symbol)

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode General Categories
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CLASSIFY(uppercase_letter)
        BOOST_SPIRIT_X3_CLASSIFY(lowercase_letter)
        BOOST_SPIRIT_X3_CLASSIFY(titlecase_letter)
        BOOST_SPIRIT_X3_CLASSIFY(modifier_letter)
        BOOST_SPIRIT_X3_CLASSIFY(other_letter)

        BOOST_SPIRIT_X3_CLASSIFY(nonspacing_mark)
        BOOST_SPIRIT_X3_CLASSIFY(enclosing_mark)
        BOOST_SPIRIT_X3_CLASSIFY(spacing_mark)

        BOOST_SPIRIT_X3_CLASSIFY(decimal_number)
        BOOST_SPIRIT_X3_CLASSIFY(letter_number)
        BOOST_SPIRIT_X3_CLASSIFY(other_number)

        BOOST_SPIRIT_X3_CLASSIFY(space_separator)
        BOOST_SPIRIT_X3_CLASSIFY(line_separator)
        BOOST_SPIRIT_X3_CLASSIFY(paragraph_separator)

        BOOST_SPIRIT_X3_CLASSIFY(control)
        BOOST_SPIRIT_X3_CLASSIFY(format)
        BOOST_SPIRIT_X3_CLASSIFY(private_use)
        BOOST_SPIRIT_X3_CLASSIFY(surrogate)
        BOOST_SPIRIT_X3_CLASSIFY(unassigned)

        BOOST_SPIRIT_X3_CLASSIFY(dash_punctuation)
        BOOST_SPIRIT_X3_CLASSIFY(open_punctuation)
        BOOST_SPIRIT_X3_CLASSIFY(close_punctuation)
        BOOST_SPIRIT_X3_CLASSIFY(connector_punctuation)
        BOOST_SPIRIT_X3_CLASSIFY(other_punctuation)
        BOOST_SPIRIT_X3_CLASSIFY(initial_punctuation)
        BOOST_SPIRIT_X3_CLASSIFY(final_punctuation)

        BOOST_SPIRIT_X3_CLASSIFY(math_symbol)
        BOOST_SPIRIT_X3_CLASSIFY(currency_symbol)
        BOOST_SPIRIT_X3_CLASSIFY(modifier_symbol)
        BOOST_SPIRIT_X3_CLASSIFY(other_symbol)

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Derived Categories
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CLASSIFY(alphabetic)
        BOOST_SPIRIT_X3_CLASSIFY(uppercase)
        BOOST_SPIRIT_X3_CLASSIFY(lowercase)
        BOOST_SPIRIT_X3_CLASSIFY(white_space)
        BOOST_SPIRIT_X3_CLASSIFY(hex_digit)
        BOOST_SPIRIT_X3_CLASSIFY(noncharacter_code_point)
        BOOST_SPIRIT_X3_CLASSIFY(default_ignorable_code_point)

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Scripts
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CLASSIFY(adlam)
        BOOST_SPIRIT_X3_CLASSIFY(caucasian_albanian)
        BOOST_SPIRIT_X3_CLASSIFY(ahom)
        BOOST_SPIRIT_X3_CLASSIFY(arabic)
        BOOST_SPIRIT_X3_CLASSIFY(imperial_aramaic)
        BOOST_SPIRIT_X3_CLASSIFY(armenian)
        BOOST_SPIRIT_X3_CLASSIFY(avestan)
        BOOST_SPIRIT_X3_CLASSIFY(balinese)
        BOOST_SPIRIT_X3_CLASSIFY(bamum)
        BOOST_SPIRIT_X3_CLASSIFY(bassa_vah)
        BOOST_SPIRIT_X3_CLASSIFY(batak)
        BOOST_SPIRIT_X3_CLASSIFY(bengali)
        BOOST_SPIRIT_X3_CLASSIFY(bhaiksuki)
        BOOST_SPIRIT_X3_CLASSIFY(bopomofo)
        BOOST_SPIRIT_X3_CLASSIFY(brahmi)
        BOOST_SPIRIT_X3_CLASSIFY(braille)
        BOOST_SPIRIT_X3_CLASSIFY(buginese)
        BOOST_SPIRIT_X3_CLASSIFY(buhid)
        BOOST_SPIRIT_X3_CLASSIFY(chakma)
        BOOST_SPIRIT_X3_CLASSIFY(canadian_aboriginal)
        BOOST_SPIRIT_X3_CLASSIFY(carian)
        BOOST_SPIRIT_X3_CLASSIFY(cham)
        BOOST_SPIRIT_X3_CLASSIFY(cherokee)
        BOOST_SPIRIT_X3_CLASSIFY(chorasmian)
        BOOST_SPIRIT_X3_CLASSIFY(coptic)
        BOOST_SPIRIT_X3_CLASSIFY(cypro_minoan)
        BOOST_SPIRIT_X3_CLASSIFY(cypriot)
        BOOST_SPIRIT_X3_CLASSIFY(cyrillic)
        BOOST_SPIRIT_X3_CLASSIFY(devanagari)
        BOOST_SPIRIT_X3_CLASSIFY(dives_akuru)
        BOOST_SPIRIT_X3_CLASSIFY(dogra)
        BOOST_SPIRIT_X3_CLASSIFY(deseret)
        BOOST_SPIRIT_X3_CLASSIFY(duployan)
        BOOST_SPIRIT_X3_CLASSIFY(egyptian_hieroglyphs)
        BOOST_SPIRIT_X3_CLASSIFY(elbasan)
        BOOST_SPIRIT_X3_CLASSIFY(elymaic)
        BOOST_SPIRIT_X3_CLASSIFY(ethiopic)
        BOOST_SPIRIT_X3_CLASSIFY(georgian)
        BOOST_SPIRIT_X3_CLASSIFY(glagolitic)
        BOOST_SPIRIT_X3_CLASSIFY(gunjala_gondi)
        BOOST_SPIRIT_X3_CLASSIFY(masaram_gondi)
        BOOST_SPIRIT_X3_CLASSIFY(gothic)
        BOOST_SPIRIT_X3_CLASSIFY(grantha)
        BOOST_SPIRIT_X3_CLASSIFY(greek)
        BOOST_SPIRIT_X3_CLASSIFY(gujarati)
        BOOST_SPIRIT_X3_CLASSIFY(gurmukhi)
        BOOST_SPIRIT_X3_CLASSIFY(hangul)
        BOOST_SPIRIT_X3_CLASSIFY(han)
        BOOST_SPIRIT_X3_CLASSIFY(hanunoo)
        BOOST_SPIRIT_X3_CLASSIFY(hatran)
        BOOST_SPIRIT_X3_CLASSIFY(hebrew)
        BOOST_SPIRIT_X3_CLASSIFY(hiragana)
        BOOST_SPIRIT_X3_CLASSIFY(anatolian_hieroglyphs)
        BOOST_SPIRIT_X3_CLASSIFY(pahawh_hmong)
        BOOST_SPIRIT_X3_CLASSIFY(nyiakeng_puachue_hmong)
        BOOST_SPIRIT_X3_CLASSIFY(katakana_or_hiragana)
        BOOST_SPIRIT_X3_CLASSIFY(old_hungarian)
        BOOST_SPIRIT_X3_CLASSIFY(old_italic)
        BOOST_SPIRIT_X3_CLASSIFY(javanese)
        BOOST_SPIRIT_X3_CLASSIFY(kayah_li)
        BOOST_SPIRIT_X3_CLASSIFY(katakana)
        BOOST_SPIRIT_X3_CLASSIFY(kawi)
        BOOST_SPIRIT_X3_CLASSIFY(kharoshthi)
        BOOST_SPIRIT_X3_CLASSIFY(khmer)
        BOOST_SPIRIT_X3_CLASSIFY(khojki)
        BOOST_SPIRIT_X3_CLASSIFY(khitan_small_script)
        BOOST_SPIRIT_X3_CLASSIFY(kannada)
        BOOST_SPIRIT_X3_CLASSIFY(kaithi)
        BOOST_SPIRIT_X3_CLASSIFY(tai_tham)
        BOOST_SPIRIT_X3_CLASSIFY(lao)
        BOOST_SPIRIT_X3_CLASSIFY(latin)
        BOOST_SPIRIT_X3_CLASSIFY(lepcha)
        BOOST_SPIRIT_X3_CLASSIFY(limbu)
        BOOST_SPIRIT_X3_CLASSIFY(linear_a)
        BOOST_SPIRIT_X3_CLASSIFY(linear_b)
        BOOST_SPIRIT_X3_CLASSIFY(lisu)
        BOOST_SPIRIT_X3_CLASSIFY(lycian)
        BOOST_SPIRIT_X3_CLASSIFY(lydian)
        BOOST_SPIRIT_X3_CLASSIFY(mahajani)
        BOOST_SPIRIT_X3_CLASSIFY(makasar)
        BOOST_SPIRIT_X3_CLASSIFY(mandaic)
        BOOST_SPIRIT_X3_CLASSIFY(manichaean)
        BOOST_SPIRIT_X3_CLASSIFY(marchen)
        BOOST_SPIRIT_X3_CLASSIFY(medefaidrin)
        BOOST_SPIRIT_X3_CLASSIFY(mende_kikakui)
        BOOST_SPIRIT_X3_CLASSIFY(meroitic_cursive)
        BOOST_SPIRIT_X3_CLASSIFY(meroitic_hieroglyphs)
        BOOST_SPIRIT_X3_CLASSIFY(malayalam)
        BOOST_SPIRIT_X3_CLASSIFY(modi)
        BOOST_SPIRIT_X3_CLASSIFY(mongolian)
        BOOST_SPIRIT_X3_CLASSIFY(mro)
        BOOST_SPIRIT_X3_CLASSIFY(meetei_mayek)
        BOOST_SPIRIT_X3_CLASSIFY(multani)
        BOOST_SPIRIT_X3_CLASSIFY(myanmar)
        BOOST_SPIRIT_X3_CLASSIFY(nag_mundari)
        BOOST_SPIRIT_X3_CLASSIFY(nandinagari)
        BOOST_SPIRIT_X3_CLASSIFY(old_north_arabian)
        BOOST_SPIRIT_X3_CLASSIFY(nabataean)
        BOOST_SPIRIT_X3_CLASSIFY(newa)
        BOOST_SPIRIT_X3_CLASSIFY(nko)
        BOOST_SPIRIT_X3_CLASSIFY(nushu)
        BOOST_SPIRIT_X3_CLASSIFY(ogham)
        BOOST_SPIRIT_X3_CLASSIFY(ol_chiki)
        BOOST_SPIRIT_X3_CLASSIFY(old_turkic)
        BOOST_SPIRIT_X3_CLASSIFY(oriya)
        BOOST_SPIRIT_X3_CLASSIFY(osage)
        BOOST_SPIRIT_X3_CLASSIFY(osmanya)
        BOOST_SPIRIT_X3_CLASSIFY(old_uyghur)
        BOOST_SPIRIT_X3_CLASSIFY(palmyrene)
        BOOST_SPIRIT_X3_CLASSIFY(pau_cin_hau)
        BOOST_SPIRIT_X3_CLASSIFY(old_permic)
        BOOST_SPIRIT_X3_CLASSIFY(phags_pa)
        BOOST_SPIRIT_X3_CLASSIFY(inscriptional_pahlavi)
        BOOST_SPIRIT_X3_CLASSIFY(psalter_pahlavi)
        BOOST_SPIRIT_X3_CLASSIFY(phoenician)
        BOOST_SPIRIT_X3_CLASSIFY(miao)
        BOOST_SPIRIT_X3_CLASSIFY(inscriptional_parthian)
        BOOST_SPIRIT_X3_CLASSIFY(rejang)
        BOOST_SPIRIT_X3_CLASSIFY(hanifi_rohingya)
        BOOST_SPIRIT_X3_CLASSIFY(runic)
        BOOST_SPIRIT_X3_CLASSIFY(samaritan)
        BOOST_SPIRIT_X3_CLASSIFY(old_south_arabian)
        BOOST_SPIRIT_X3_CLASSIFY(saurashtra)
        BOOST_SPIRIT_X3_CLASSIFY(signwriting)
        BOOST_SPIRIT_X3_CLASSIFY(shavian)
        BOOST_SPIRIT_X3_CLASSIFY(sharada)
        BOOST_SPIRIT_X3_CLASSIFY(siddham)
        BOOST_SPIRIT_X3_CLASSIFY(khudawadi)
        BOOST_SPIRIT_X3_CLASSIFY(sinhala)
        BOOST_SPIRIT_X3_CLASSIFY(sogdian)
        BOOST_SPIRIT_X3_CLASSIFY(old_sogdian)
        BOOST_SPIRIT_X3_CLASSIFY(sora_sompeng)
        BOOST_SPIRIT_X3_CLASSIFY(soyombo)
        BOOST_SPIRIT_X3_CLASSIFY(sundanese)
        BOOST_SPIRIT_X3_CLASSIFY(syloti_nagri)
        BOOST_SPIRIT_X3_CLASSIFY(syriac)
        BOOST_SPIRIT_X3_CLASSIFY(tagbanwa)
        BOOST_SPIRIT_X3_CLASSIFY(takri)
        BOOST_SPIRIT_X3_CLASSIFY(tai_le)
        BOOST_SPIRIT_X3_CLASSIFY(new_tai_lue)
        BOOST_SPIRIT_X3_CLASSIFY(tamil)
        BOOST_SPIRIT_X3_CLASSIFY(tangut)
        BOOST_SPIRIT_X3_CLASSIFY(tai_viet)
        BOOST_SPIRIT_X3_CLASSIFY(telugu)
        BOOST_SPIRIT_X3_CLASSIFY(tifinagh)
        BOOST_SPIRIT_X3_CLASSIFY(tagalog)
        BOOST_SPIRIT_X3_CLASSIFY(thaana)
        BOOST_SPIRIT_X3_CLASSIFY(thai)
        BOOST_SPIRIT_X3_CLASSIFY(tibetan)
        BOOST_SPIRIT_X3_CLASSIFY(tirhuta)
        BOOST_SPIRIT_X3_CLASSIFY(tangsa)
        BOOST_SPIRIT_X3_CLASSIFY(toto)
        BOOST_SPIRIT_X3_CLASSIFY(ugaritic)
        BOOST_SPIRIT_X3_CLASSIFY(vai)
        BOOST_SPIRIT_X3_CLASSIFY(vithkuqi)
        BOOST_SPIRIT_X3_CLASSIFY(warang_citi)
        BOOST_SPIRIT_X3_CLASSIFY(wancho)
        BOOST_SPIRIT_X3_CLASSIFY(old_persian)
        BOOST_SPIRIT_X3_CLASSIFY(cuneiform)
        BOOST_SPIRIT_X3_CLASSIFY(yezidi)
        BOOST_SPIRIT_X3_CLASSIFY(yi)
        BOOST_SPIRIT_X3_CLASSIFY(zanabazar_square)
        BOOST_SPIRIT_X3_CLASSIFY(inherited)
        BOOST_SPIRIT_X3_CLASSIFY(common)
        BOOST_SPIRIT_X3_CLASSIFY(unknown)

#undef BOOST_SPIRIT_X3_BASIC_CLASSIFY
#undef BOOST_SPIRIT_X3_CLASSIFY
    };

    template <typename Tag>
    struct unicode_char_class
      : char_parser<unicode_char_class<Tag>>
    {
        typedef char_encoding::unicode encoding;
        typedef Tag tag;
        typedef typename encoding::char_type char_type;
        typedef char_type attribute_type;
        static bool const has_attribute = true;

        template <typename Char, typename Context>
        bool test(Char ch, Context const&) const
        {
            return encoding::ischar(ch) && unicode_char_class_base::is(tag(), ch);
        }
    };

#define BOOST_SPIRIT_X3_CHAR_CLASS(name)                                         \
    typedef unicode_char_class<name##_tag> name##_type;                          \
    constexpr name##_type name = name##_type();                                  \
    /***/

    namespace unicode
    {
        typedef any_char<char_encoding::unicode> char_type;
        constexpr auto char_ = char_type{};

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Major Categories
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CHAR_CLASS(alnum)
        BOOST_SPIRIT_X3_CHAR_CLASS(alpha)
        BOOST_SPIRIT_X3_CHAR_CLASS(digit)
        BOOST_SPIRIT_X3_CHAR_CLASS(xdigit)
        BOOST_SPIRIT_X3_CHAR_CLASS(cntrl)
        BOOST_SPIRIT_X3_CHAR_CLASS(graph)
        BOOST_SPIRIT_X3_CHAR_CLASS(lower)
        BOOST_SPIRIT_X3_CHAR_CLASS(print)
        BOOST_SPIRIT_X3_CHAR_CLASS(punct)
        BOOST_SPIRIT_X3_CHAR_CLASS(space)
        BOOST_SPIRIT_X3_CHAR_CLASS(blank)
        BOOST_SPIRIT_X3_CHAR_CLASS(upper)

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Major Categories
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CHAR_CLASS(letter)
        BOOST_SPIRIT_X3_CHAR_CLASS(mark)
        BOOST_SPIRIT_X3_CHAR_CLASS(number)
        BOOST_SPIRIT_X3_CHAR_CLASS(separator)
        BOOST_SPIRIT_X3_CHAR_CLASS(other)
        BOOST_SPIRIT_X3_CHAR_CLASS(punctuation)
        BOOST_SPIRIT_X3_CHAR_CLASS(symbol)

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode General Categories
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CHAR_CLASS(uppercase_letter)
        BOOST_SPIRIT_X3_CHAR_CLASS(lowercase_letter)
        BOOST_SPIRIT_X3_CHAR_CLASS(titlecase_letter)
        BOOST_SPIRIT_X3_CHAR_CLASS(modifier_letter)
        BOOST_SPIRIT_X3_CHAR_CLASS(other_letter)

        BOOST_SPIRIT_X3_CHAR_CLASS(nonspacing_mark)
        BOOST_SPIRIT_X3_CHAR_CLASS(enclosing_mark)
        BOOST_SPIRIT_X3_CHAR_CLASS(spacing_mark)

        BOOST_SPIRIT_X3_CHAR_CLASS(decimal_number)
        BOOST_SPIRIT_X3_CHAR_CLASS(letter_number)
        BOOST_SPIRIT_X3_CHAR_CLASS(other_number)

        BOOST_SPIRIT_X3_CHAR_CLASS(space_separator)
        BOOST_SPIRIT_X3_CHAR_CLASS(line_separator)
        BOOST_SPIRIT_X3_CHAR_CLASS(paragraph_separator)

        BOOST_SPIRIT_X3_CHAR_CLASS(control)
        BOOST_SPIRIT_X3_CHAR_CLASS(format)
        BOOST_SPIRIT_X3_CHAR_CLASS(private_use)
        BOOST_SPIRIT_X3_CHAR_CLASS(surrogate)
        BOOST_SPIRIT_X3_CHAR_CLASS(unassigned)

        BOOST_SPIRIT_X3_CHAR_CLASS(dash_punctuation)
        BOOST_SPIRIT_X3_CHAR_CLASS(open_punctuation)
        BOOST_SPIRIT_X3_CHAR_CLASS(close_punctuation)
        BOOST_SPIRIT_X3_CHAR_CLASS(connector_punctuation)
        BOOST_SPIRIT_X3_CHAR_CLASS(other_punctuation)
        BOOST_SPIRIT_X3_CHAR_CLASS(initial_punctuation)
        BOOST_SPIRIT_X3_CHAR_CLASS(final_punctuation)

        BOOST_SPIRIT_X3_CHAR_CLASS(math_symbol)
        BOOST_SPIRIT_X3_CHAR_CLASS(currency_symbol)
        BOOST_SPIRIT_X3_CHAR_CLASS(modifier_symbol)
        BOOST_SPIRIT_X3_CHAR_CLASS(other_symbol)

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Derived Categories
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CHAR_CLASS(alphabetic)
        BOOST_SPIRIT_X3_CHAR_CLASS(uppercase)
        BOOST_SPIRIT_X3_CHAR_CLASS(lowercase)
        BOOST_SPIRIT_X3_CHAR_CLASS(white_space)
        BOOST_SPIRIT_X3_CHAR_CLASS(hex_digit)
        BOOST_SPIRIT_X3_CHAR_CLASS(noncharacter_code_point)
        BOOST_SPIRIT_X3_CHAR_CLASS(default_ignorable_code_point)

    ///////////////////////////////////////////////////////////////////////////
    //  Unicode Scripts
    ///////////////////////////////////////////////////////////////////////////
        BOOST_SPIRIT_X3_CHAR_CLASS(adlam)
        BOOST_SPIRIT_X3_CHAR_CLASS(caucasian_albanian)
        BOOST_SPIRIT_X3_CHAR_CLASS(ahom)
        BOOST_SPIRIT_X3_CHAR_CLASS(arabic)
        BOOST_SPIRIT_X3_CHAR_CLASS(imperial_aramaic)
        BOOST_SPIRIT_X3_CHAR_CLASS(armenian)
        BOOST_SPIRIT_X3_CHAR_CLASS(avestan)
        BOOST_SPIRIT_X3_CHAR_CLASS(balinese)
        BOOST_SPIRIT_X3_CHAR_CLASS(bamum)
        BOOST_SPIRIT_X3_CHAR_CLASS(bassa_vah)
        BOOST_SPIRIT_X3_CHAR_CLASS(batak)
        BOOST_SPIRIT_X3_CHAR_CLASS(bengali)
        BOOST_SPIRIT_X3_CHAR_CLASS(bhaiksuki)
        BOOST_SPIRIT_X3_CHAR_CLASS(bopomofo)
        BOOST_SPIRIT_X3_CHAR_CLASS(brahmi)
        BOOST_SPIRIT_X3_CHAR_CLASS(braille)
        BOOST_SPIRIT_X3_CHAR_CLASS(buginese)
        BOOST_SPIRIT_X3_CHAR_CLASS(buhid)
        BOOST_SPIRIT_X3_CHAR_CLASS(chakma)
        BOOST_SPIRIT_X3_CHAR_CLASS(canadian_aboriginal)
        BOOST_SPIRIT_X3_CHAR_CLASS(carian)
        BOOST_SPIRIT_X3_CHAR_CLASS(cham)
        BOOST_SPIRIT_X3_CHAR_CLASS(cherokee)
        BOOST_SPIRIT_X3_CHAR_CLASS(chorasmian)
        BOOST_SPIRIT_X3_CHAR_CLASS(coptic)
        BOOST_SPIRIT_X3_CHAR_CLASS(cypro_minoan)
        BOOST_SPIRIT_X3_CHAR_CLASS(cypriot)
        BOOST_SPIRIT_X3_CHAR_CLASS(cyrillic)
        BOOST_SPIRIT_X3_CHAR_CLASS(devanagari)
        BOOST_SPIRIT_X3_CHAR_CLASS(dives_akuru)
        BOOST_SPIRIT_X3_CHAR_CLASS(dogra)
        BOOST_SPIRIT_X3_CHAR_CLASS(deseret)
        BOOST_SPIRIT_X3_CHAR_CLASS(duployan)
        BOOST_SPIRIT_X3_CHAR_CLASS(egyptian_hieroglyphs)
        BOOST_SPIRIT_X3_CHAR_CLASS(elbasan)
        BOOST_SPIRIT_X3_CHAR_CLASS(elymaic)
        BOOST_SPIRIT_X3_CHAR_CLASS(ethiopic)
        BOOST_SPIRIT_X3_CHAR_CLASS(georgian)
        BOOST_SPIRIT_X3_CHAR_CLASS(glagolitic)
        BOOST_SPIRIT_X3_CHAR_CLASS(gunjala_gondi)
        BOOST_SPIRIT_X3_CHAR_CLASS(masaram_gondi)
        BOOST_SPIRIT_X3_CHAR_CLASS(gothic)
        BOOST_SPIRIT_X3_CHAR_CLASS(grantha)
        BOOST_SPIRIT_X3_CHAR_CLASS(greek)
        BOOST_SPIRIT_X3_CHAR_CLASS(gujarati)
        BOOST_SPIRIT_X3_CHAR_CLASS(gurmukhi)
        BOOST_SPIRIT_X3_CHAR_CLASS(hangul)
        BOOST_SPIRIT_X3_CHAR_CLASS(han)
        BOOST_SPIRIT_X3_CHAR_CLASS(hanunoo)
        BOOST_SPIRIT_X3_CHAR_CLASS(hatran)
        BOOST_SPIRIT_X3_CHAR_CLASS(hebrew)
        BOOST_SPIRIT_X3_CHAR_CLASS(hiragana)
        BOOST_SPIRIT_X3_CHAR_CLASS(anatolian_hieroglyphs)
        BOOST_SPIRIT_X3_CHAR_CLASS(pahawh_hmong)
        BOOST_SPIRIT_X3_CHAR_CLASS(nyiakeng_puachue_hmong)
        BOOST_SPIRIT_X3_CHAR_CLASS(katakana_or_hiragana)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_hungarian)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_italic)
        BOOST_SPIRIT_X3_CHAR_CLASS(javanese)
        BOOST_SPIRIT_X3_CHAR_CLASS(kayah_li)
        BOOST_SPIRIT_X3_CHAR_CLASS(katakana)
        BOOST_SPIRIT_X3_CHAR_CLASS(kawi)
        BOOST_SPIRIT_X3_CHAR_CLASS(kharoshthi)
        BOOST_SPIRIT_X3_CHAR_CLASS(khmer)
        BOOST_SPIRIT_X3_CHAR_CLASS(khojki)
        BOOST_SPIRIT_X3_CHAR_CLASS(khitan_small_script)
        BOOST_SPIRIT_X3_CHAR_CLASS(kannada)
        BOOST_SPIRIT_X3_CHAR_CLASS(kaithi)
        BOOST_SPIRIT_X3_CHAR_CLASS(tai_tham)
        BOOST_SPIRIT_X3_CHAR_CLASS(lao)
        BOOST_SPIRIT_X3_CHAR_CLASS(latin)
        BOOST_SPIRIT_X3_CHAR_CLASS(lepcha)
        BOOST_SPIRIT_X3_CHAR_CLASS(limbu)
        BOOST_SPIRIT_X3_CHAR_CLASS(linear_a)
        BOOST_SPIRIT_X3_CHAR_CLASS(linear_b)
        BOOST_SPIRIT_X3_CHAR_CLASS(lisu)
        BOOST_SPIRIT_X3_CHAR_CLASS(lycian)
        BOOST_SPIRIT_X3_CHAR_CLASS(lydian)
        BOOST_SPIRIT_X3_CHAR_CLASS(mahajani)
        BOOST_SPIRIT_X3_CHAR_CLASS(makasar)
        BOOST_SPIRIT_X3_CHAR_CLASS(mandaic)
        BOOST_SPIRIT_X3_CHAR_CLASS(manichaean)
        BOOST_SPIRIT_X3_CHAR_CLASS(marchen)
        BOOST_SPIRIT_X3_CHAR_CLASS(medefaidrin)
        BOOST_SPIRIT_X3_CHAR_CLASS(mende_kikakui)
        BOOST_SPIRIT_X3_CHAR_CLASS(meroitic_cursive)
        BOOST_SPIRIT_X3_CHAR_CLASS(meroitic_hieroglyphs)
        BOOST_SPIRIT_X3_CHAR_CLASS(malayalam)
        BOOST_SPIRIT_X3_CHAR_CLASS(modi)
        BOOST_SPIRIT_X3_CHAR_CLASS(mongolian)
        BOOST_SPIRIT_X3_CHAR_CLASS(mro)
        BOOST_SPIRIT_X3_CHAR_CLASS(meetei_mayek)
        BOOST_SPIRIT_X3_CHAR_CLASS(multani)
        BOOST_SPIRIT_X3_CHAR_CLASS(myanmar)
        BOOST_SPIRIT_X3_CHAR_CLASS(nag_mundari)
        BOOST_SPIRIT_X3_CHAR_CLASS(nandinagari)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_north_arabian)
        BOOST_SPIRIT_X3_CHAR_CLASS(nabataean)
        BOOST_SPIRIT_X3_CHAR_CLASS(newa)
        BOOST_SPIRIT_X3_CHAR_CLASS(nko)
        BOOST_SPIRIT_X3_CHAR_CLASS(nushu)
        BOOST_SPIRIT_X3_CHAR_CLASS(ogham)
        BOOST_SPIRIT_X3_CHAR_CLASS(ol_chiki)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_turkic)
        BOOST_SPIRIT_X3_CHAR_CLASS(oriya)
        BOOST_SPIRIT_X3_CHAR_CLASS(osage)
        BOOST_SPIRIT_X3_CHAR_CLASS(osmanya)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_uyghur)
        BOOST_SPIRIT_X3_CHAR_CLASS(palmyrene)
        BOOST_SPIRIT_X3_CHAR_CLASS(pau_cin_hau)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_permic)
        BOOST_SPIRIT_X3_CHAR_CLASS(phags_pa)
        BOOST_SPIRIT_X3_CHAR_CLASS(inscriptional_pahlavi)
        BOOST_SPIRIT_X3_CHAR_CLASS(psalter_pahlavi)
        BOOST_SPIRIT_X3_CHAR_CLASS(phoenician)
        BOOST_SPIRIT_X3_CHAR_CLASS(miao)
        BOOST_SPIRIT_X3_CHAR_CLASS(inscriptional_parthian)
        BOOST_SPIRIT_X3_CHAR_CLASS(rejang)
        BOOST_SPIRIT_X3_CHAR_CLASS(hanifi_rohingya)
        BOOST_SPIRIT_X3_CHAR_CLASS(runic)
        BOOST_SPIRIT_X3_CHAR_CLASS(samaritan)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_south_arabian)
        BOOST_SPIRIT_X3_CHAR_CLASS(saurashtra)
        BOOST_SPIRIT_X3_CHAR_CLASS(signwriting)
        BOOST_SPIRIT_X3_CHAR_CLASS(shavian)
        BOOST_SPIRIT_X3_CHAR_CLASS(sharada)
        BOOST_SPIRIT_X3_CHAR_CLASS(siddham)
        BOOST_SPIRIT_X3_CHAR_CLASS(khudawadi)
        BOOST_SPIRIT_X3_CHAR_CLASS(sinhala)
        BOOST_SPIRIT_X3_CHAR_CLASS(sogdian)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_sogdian)
        BOOST_SPIRIT_X3_CHAR_CLASS(sora_sompeng)
        BOOST_SPIRIT_X3_CHAR_CLASS(soyombo)
        BOOST_SPIRIT_X3_CHAR_CLASS(sundanese)
        BOOST_SPIRIT_X3_CHAR_CLASS(syloti_nagri)
        BOOST_SPIRIT_X3_CHAR_CLASS(syriac)
        BOOST_SPIRIT_X3_CHAR_CLASS(tagbanwa)
        BOOST_SPIRIT_X3_CHAR_CLASS(takri)
        BOOST_SPIRIT_X3_CHAR_CLASS(tai_le)
        BOOST_SPIRIT_X3_CHAR_CLASS(new_tai_lue)
        BOOST_SPIRIT_X3_CHAR_CLASS(tamil)
        BOOST_SPIRIT_X3_CHAR_CLASS(tangut)
        BOOST_SPIRIT_X3_CHAR_CLASS(tai_viet)
        BOOST_SPIRIT_X3_CHAR_CLASS(telugu)
        BOOST_SPIRIT_X3_CHAR_CLASS(tifinagh)
        BOOST_SPIRIT_X3_CHAR_CLASS(tagalog)
        BOOST_SPIRIT_X3_CHAR_CLASS(thaana)
        BOOST_SPIRIT_X3_CHAR_CLASS(thai)
        BOOST_SPIRIT_X3_CHAR_CLASS(tibetan)
        BOOST_SPIRIT_X3_CHAR_CLASS(tirhuta)
        BOOST_SPIRIT_X3_CHAR_CLASS(tangsa)
        BOOST_SPIRIT_X3_CHAR_CLASS(toto)
        BOOST_SPIRIT_X3_CHAR_CLASS(ugaritic)
        BOOST_SPIRIT_X3_CHAR_CLASS(vai)
        BOOST_SPIRIT_X3_CHAR_CLASS(vithkuqi)
        BOOST_SPIRIT_X3_CHAR_CLASS(warang_citi)
        BOOST_SPIRIT_X3_CHAR_CLASS(wancho)
        BOOST_SPIRIT_X3_CHAR_CLASS(old_persian)
        BOOST_SPIRIT_X3_CHAR_CLASS(cuneiform)
        BOOST_SPIRIT_X3_CHAR_CLASS(yezidi)
        BOOST_SPIRIT_X3_CHAR_CLASS(yi)
        BOOST_SPIRIT_X3_CHAR_CLASS(zanabazar_square)
        BOOST_SPIRIT_X3_CHAR_CLASS(inherited)
        BOOST_SPIRIT_X3_CHAR_CLASS(common)
        BOOST_SPIRIT_X3_CHAR_CLASS(unknown)
    }

#undef BOOST_SPIRIT_X3_CHAR_CLASS

}}}

#endif
