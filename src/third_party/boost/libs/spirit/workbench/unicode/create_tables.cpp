/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#include <boost/spirit/include/qi.hpp>
#include <boost/phoenix.hpp>
#include <boost/unordered_map.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/cstdint.hpp>
#include <boost/foreach.hpp>
#include <boost/array.hpp>
#include <boost/scoped_array.hpp>
#include <boost/range/iterator_range.hpp>

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <algorithm>
#include <string>
#include <map>

// We place the data here. Each line comprises various fields
typedef std::vector<std::string> ucd_line;
typedef std::vector<ucd_line> ucd_vector;
typedef std::vector<ucd_line>::iterator ucd_iterator;

// spirit and phoenix using declarations
using boost::spirit::qi::parse;
using boost::spirit::qi::hex;
using boost::spirit::qi::char_;
using boost::spirit::qi::eol;
using boost::spirit::qi::rule;
using boost::spirit::qi::omit;
using boost::spirit::qi::_1;
using boost::spirit::qi::_val;
using boost::phoenix::push_back;
using boost::phoenix::ref;

// basic unsigned types
using boost::uint8_t;
using boost::uint16_t;
using boost::uint32_t;

enum code_action
{
    assign_code_value,
    assign_property,
    append_property
};

// a char range
struct ucd_range
{
    ucd_range(uint32_t start, uint32_t finish)
        : start(start), finish(finish) {}

    // we need this so we can use ucd_range as a multimap key
    friend bool operator<(ucd_range const& a, ucd_range const& b)
    {
        return a.start < b.start;
    }

    uint32_t start;
    uint32_t finish;
};

class ucd_info
{
public:

    ucd_info(char const* filename)
    {
        std::ifstream in(filename, std::ios_base::in);
        if (!in)
        {
            std::cerr << "Error: Could not open input file: "
                << filename << std::endl;
        }
        else
        {
            std::string data;               // We will read the contents here.
            in.unsetf(std::ios::skipws);    // No white space skipping!
            std::copy(
                std::istream_iterator<char>(in),
                std::istream_iterator<char>(),
                std::back_inserter(data));

            typedef std::string::const_iterator iterator_type;
            iterator_type f = data.begin();
            iterator_type l = data.end();

            rule<iterator_type> endl = -('#' >> *(char_-eol)) >> eol;
            rule<iterator_type, std::string()> field = *(char_-(';'|endl)) >> (';'|&endl);
            rule<iterator_type, ucd_line()> line = +(field-endl) >> endl;
            rule<iterator_type, std::vector<ucd_line>()> file = +(endl | line[push_back(_val, _1)]);

            parse(f, l, file, info);
        }
    }

    template <typename Array>
    void collect(Array& data, int field, code_action action) const
    {
        BOOST_ASSERT(!info.empty());
        ucd_vector::const_iterator current = info.begin();
        ucd_vector::const_iterator end = info.end();

        while (current != end)
        {
            std::string range = (*current)[0];
            boost::trim(range);

            std::string::const_iterator f = range.begin();
            std::string::const_iterator l = range.end();

            // get the code-point range
            uint32_t start;
            uint32_t finish;
            parse(f, l, hex[ref(start) = ref(finish) = _1] >> -(".." >> hex[ref(finish) = _1]));

            // special case for UnicodeData.txt ranges:
            if ((*current)[1].find("First>") != std::string::npos)
            {
                ++current;
                BOOST_ASSERT(current != end);
                BOOST_ASSERT((*current)[1].find("Last>") != std::string::npos);

                std::string range = (*current)[0];
                boost::trim(range);
                f = range.begin();
                l = range.end();

                parse(f, l, hex[ref(finish) = _1]);
            }

            std::string code;
            if (field < int(current->size()))
                code = (*current)[field];
            boost::trim(code);

            if (assign_code_value != action) // code for properties
            {
                // Only collect properties we are interested in
                if (!ignore_property(code))
                {
                    if (assign_property == action)
                    {
                        for (uint32_t i = start; i <= finish; ++i)
                            data[i] = map_property(code);
                    }
                    else
                    {
                        for (uint32_t i = start; i <= finish; ++i)
                            data[i] |= map_property(code);
                    }
                }
            }
            else // code for actual numeric values
            {
                for (uint32_t i = start; i <= finish; ++i)
                {
                    if (code.empty())
                    {
                        data[i] = 0; // signal that this code maps to itself
                    }
                    else
                    {
                        f = code.begin();
                        l = code.end();
                        parse(f, l, hex, data[i]);
                    }
                }
            }
            ++current;
        }
    }

    static bool ignore_property(std::string const& p)
    {
        // We don't handle all properties
        std::map<std::string, int>& pm = get_property_map();
        std::map<std::string, int>::iterator i = pm.find(p);
        return i == pm.end();
    }

    static int
    map_property(std::string const& p)
    {
        std::map<std::string, int>& pm = get_property_map();
        std::map<std::string, int>::iterator i = pm.find(p);
        BOOST_ASSERT(i != pm.end());
        return i->second;
    }

private:

    static std::map<std::string, int>&
    get_property_map()
    {
        // The properties we are interested in:
        static std::map<std::string, int> map;
        if (map.empty())
        {
            // General_Category
            map["Lu"] = 0;
            map["Ll"] = 1;
            map["Lt"] = 2;
            map["Lm"] = 3;
            map["Lo"] = 4;

            map["Mn"] = 8;
            map["Me"] = 9;
            map["Mc"] = 10;

            map["Nd"] = 16;
            map["Nl"] = 17;
            map["No"] = 18;

            map["Zs"] = 24;
            map["Zl"] = 25;
            map["Zp"] = 26;

            map["Cc"] = 32;
            map["Cf"] = 33;
            map["Co"] = 34;
            map["Cs"] = 35;
            map["Cn"] = 36;

            map["Pd"] = 40;
            map["Ps"] = 41;
            map["Pe"] = 42;
            map["Pc"] = 43;
            map["Po"] = 44;
            map["Pi"] = 45;
            map["Pf"] = 46;

            map["Sm"] = 48;
            map["Sc"] = 49;
            map["Sk"] = 50;
            map["So"] = 51;

            // Derived Properties.
            map["Alphabetic"] = 64;
            map["Uppercase"] = 128;
            map["Lowercase"] = 256;
            map["White_Space"] = 512;
            map["Hex_Digit"] = 1024;
            map["Noncharacter_Code_Point"] = 2048;
            map["Default_Ignorable_Code_Point"] = 4096;

            // Script
            int i = 0;
            map["Adlam"] = i++;
            map["Caucasian_Albanian"] = i++;
            map["Ahom"] = i++;
            map["Arabic"] = i++;
            map["Imperial_Aramaic"] = i++;
            map["Armenian"] = i++;
            map["Avestan"] = i++;
            map["Balinese"] = i++;
            map["Bamum"] = i++;
            map["Bassa_Vah"] = i++;
            map["Batak"] = i++;
            map["Bengali"] = i++;
            map["Bhaiksuki"] = i++;
            map["Bopomofo"] = i++;
            map["Brahmi"] = i++;
            map["Braille"] = i++;
            map["Buginese"] = i++;
            map["Buhid"] = i++;
            map["Chakma"] = i++;
            map["Canadian_Aboriginal"] = i++;
            map["Carian"] = i++;
            map["Cham"] = i++;
            map["Cherokee"] = i++;
            map["Chorasmian"] = i++;
            map["Coptic"] = i++;
            map["Cypro_Minoan"] = i++;
            map["Cypriot"] = i++;
            map["Cyrillic"] = i++;
            map["Devanagari"] = i++;
            map["Dives_Akuru"] = i++;
            map["Dogra"] = i++;
            map["Deseret"] = i++;
            map["Duployan"] = i++;
            map["Egyptian_Hieroglyphs"] = i++;
            map["Elbasan"] = i++;
            map["Elymaic"] = i++;
            map["Ethiopic"] = i++;
            map["Georgian"] = i++;
            map["Glagolitic"] = i++;
            map["Gunjala_Gondi"] = i++;
            map["Masaram_Gondi"] = i++;
            map["Gothic"] = i++;
            map["Grantha"] = i++;
            map["Greek"] = i++;
            map["Gujarati"] = i++;
            map["Gurmukhi"] = i++;
            map["Hangul"] = i++;
            map["Han"] = i++;
            map["Hanunoo"] = i++;
            map["Hatran"] = i++;
            map["Hebrew"] = i++;
            map["Hiragana"] = i++;
            map["Anatolian_Hieroglyphs"] = i++;
            map["Pahawh_Hmong"] = i++;
            map["Nyiakeng_Puachue_Hmong"] = i++;
            map["Katakana_Or_Hiragana"] = i++;
            map["Old_Hungarian"] = i++;
            map["Old_Italic"] = i++;
            map["Javanese"] = i++;
            map["Kayah_Li"] = i++;
            map["Katakana"] = i++;
            map["Kawi"] = i++;
            map["Kharoshthi"] = i++;
            map["Khmer"] = i++;
            map["Khojki"] = i++;
            map["Khitan_Small_Script"] = i++;
            map["Kannada"] = i++;
            map["Kaithi"] = i++;
            map["Tai_Tham"] = i++;
            map["Lao"] = i++;
            map["Latin"] = i++;
            map["Lepcha"] = i++;
            map["Limbu"] = i++;
            map["Linear_A"] = i++;
            map["Linear_B"] = i++;
            map["Lisu"] = i++;
            map["Lycian"] = i++;
            map["Lydian"] = i++;
            map["Mahajani"] = i++;
            map["Makasar"] = i++;
            map["Mandaic"] = i++;
            map["Manichaean"] = i++;
            map["Marchen"] = i++;
            map["Medefaidrin"] = i++;
            map["Mende_Kikakui"] = i++;
            map["Meroitic_Cursive"] = i++;
            map["Meroitic_Hieroglyphs"] = i++;
            map["Malayalam"] = i++;
            map["Modi"] = i++;
            map["Mongolian"] = i++;
            map["Mro"] = i++;
            map["Meetei_Mayek"] = i++;
            map["Multani"] = i++;
            map["Myanmar"] = i++;
            map["Nag_Mundari"] = i++;
            map["Nandinagari"] = i++;
            map["Old_North_Arabian"] = i++;
            map["Nabataean"] = i++;
            map["Newa"] = i++;
            map["Nko"] = i++;
            map["Nushu"] = i++;
            map["Ogham"] = i++;
            map["Ol_Chiki"] = i++;
            map["Old_Turkic"] = i++;
            map["Oriya"] = i++;
            map["Osage"] = i++;
            map["Osmanya"] = i++;
            map["Old_Uyghur"] = i++;
            map["Palmyrene"] = i++;
            map["Pau_Cin_Hau"] = i++;
            map["Old_Permic"] = i++;
            map["Phags_Pa"] = i++;
            map["Inscriptional_Pahlavi"] = i++;
            map["Psalter_Pahlavi"] = i++;
            map["Phoenician"] = i++;
            map["Miao"] = i++;
            map["Inscriptional_Parthian"] = i++;
            map["Rejang"] = i++;
            map["Hanifi_Rohingya"] = i++;
            map["Runic"] = i++;
            map["Samaritan"] = i++;
            map["Old_South_Arabian"] = i++;
            map["Saurashtra"] = i++;
            map["SignWriting"] = i++;
            map["Shavian"] = i++;
            map["Sharada"] = i++;
            map["Siddham"] = i++;
            map["Khudawadi"] = i++;
            map["Sinhala"] = i++;
            map["Sogdian"] = i++;
            map["Old_Sogdian"] = i++;
            map["Sora_Sompeng"] = i++;
            map["Soyombo"] = i++;
            map["Sundanese"] = i++;
            map["Syloti_Nagri"] = i++;
            map["Syriac"] = i++;
            map["Tagbanwa"] = i++;
            map["Takri"] = i++;
            map["Tai_Le"] = i++;
            map["New_Tai_Lue"] = i++;
            map["Tamil"] = i++;
            map["Tangut"] = i++;
            map["Tai_Viet"] = i++;
            map["Telugu"] = i++;
            map["Tifinagh"] = i++;
            map["Tagalog"] = i++;
            map["Thaana"] = i++;
            map["Thai"] = i++;
            map["Tibetan"] = i++;
            map["Tirhuta"] = i++;
            map["Tangsa"] = i++;
            map["Toto"] = i++;
            map["Ugaritic"] = i++;
            map["Vai"] = i++;
            map["Vithkuqi"] = i++;
            map["Warang_Citi"] = i++;
            map["Wancho"] = i++;
            map["Old_Persian"] = i++;
            map["Cuneiform"] = i++;
            map["Yezidi"] = i++;
            map["Yi"] = i++;
            map["Zanabazar_Square"] = i++;
            map["Inherited"] = i++;
            map["Common"] = i++;
            map["Unknown"] = i++;
        }
        return map;
    }

    ucd_vector info;
};

template <typename T, uint32_t block_size_ = 256>
class ucd_table_builder
{
public:

    static uint32_t const block_size = block_size_;
    static uint32_t const full_span = 0x110000;
    typedef T value_type;

    ucd_table_builder(T default_value = 0) : p(new T[full_span])
    {
        for (uint32_t i = 0; i < full_span; ++i)
            p[i] = default_value;
    }

    void collect(char const* filename, int field, code_action action)
    {
        std::cout << "collecting " << filename << std::endl;
        ucd_info info(filename);
        info.collect(p, field, action);
    }

    void build(std::vector<uint8_t>& stage1, std::vector<T const*>& stage2)
    {
        std::cout << "building tables" << std::endl;
        std::map<block_ptr, std::vector<T const*> > blocks;
        for (T const* i = p.get(); i < (p.get() + full_span); i += block_size)
            blocks[block_ptr(i)].push_back(i);

        // Not enough bits to store the block indices.
        BOOST_ASSERT(blocks.size() < (1 << (sizeof(uint8_t) * 8)));

        typedef std::pair<block_ptr, std::vector<T const*> > blocks_value_type;
        std::map<T const*, std::vector<T const*> > sorted_blocks;
        BOOST_FOREACH(blocks_value_type const& val, blocks)
        {
            sorted_blocks[val.first.p] = val.second;
        }

        stage1.clear();
        stage1.reserve(full_span / block_size);
        stage1.resize(full_span / block_size);
        stage2.clear();
        stage2.reserve(blocks.size());

        typedef std::pair<T const*, std::vector<T const*> > sorted_blocks_value_type;
        BOOST_FOREACH(sorted_blocks_value_type const& val, sorted_blocks)
        {
            stage2.push_back(val.first);
            BOOST_FOREACH(T const* val2, val.second)
            {
                stage1[(val2 - p.get()) / block_size] = stage2.size() - 1;
            }
        }
    }

private:

    struct block_ptr
    {
        block_ptr(T const* p) : p(p) {}

        friend bool operator<(block_ptr a, block_ptr b)
        {
            return std::lexicographical_compare(
                a.p, a.p + block_size, b.p, b.p + block_size);
        }

        T const* p;
    };

    boost::scoped_array<T> p;
};

template <typename Out>
void print_tab(Out& out, int tab)
{
    for (int i = 0; i < tab; ++i)
        out << ' ';
}

template <typename Out, typename C>
void print_table(Out& out, C const& c, bool trailing_comma, int width = 4, int group = 16)
{
    int const tab = 4;
    typename C::size_type size = c.size();
    BOOST_ASSERT(size > 1);
    print_tab(out, tab);
    out << std::setw(width) << int(c[0]);
    for (typename C::size_type i = 1; i < size; ++i)
    {
        out << ", ";
        if ((i % group) == 0)
        {
            out << std::endl;
            print_tab(out, tab);
        }
        out << std::setw(width) << int(c[i]);
    }

    if (trailing_comma)
        out << ", " << std::endl;
}

template <typename Out>
void print_head(Out& out)
{
    out
        << "/*=============================================================================\n"
        << "    Copyright (c) 2001-2011 Joel de Guzman\n"
        << "\n"
        << "    Distributed under the Boost Software License, Version 1.0. (See accompanying\n"
        << "    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)\n"
        << "\n"
        << "    AUTOGENERATED. DO NOT EDIT!!!\n"
        << "==============================================================================*/\n"
        << "#include <boost/cstdint.hpp>\n"
        << "\n"
        << "namespace boost { namespace spirit { namespace ucd { namespace detail\n"
        << "{"
        ;
}

template <typename Out>
void print_tail(Out& out)
{
    out
        << "\n"
        << "}}}} // namespace boost::spirit::unicode::detail\n"
        ;
}

char const* get_int_type_name(int size)
{
    switch (size)
    {
        case 1: return "::boost::uint8_t";
        case 2: return "::boost::uint16_t";
        case 4: return "::boost::uint32_t";
        case 5: return "::boost::uint64_t";
        default: BOOST_ASSERT(false); return 0; // invalid size
    };
}

template <typename Out, typename Builder>
void print_file(Out& out, Builder& builder, int field_width, char const* name)
{
    std::cout << "Generating " << name << " tables" << std::endl;

    uint32_t const block_size = Builder::block_size;
    typedef typename Builder::value_type value_type;
    print_head(out);

    std::vector<uint8_t> stage1;
    std::vector<value_type const*> stage2;
    builder.build(stage1, stage2);
    std::cout << "Block Size: " << block_size << std::endl;
    std::cout << "Total Bytes: "
        << stage1.size()+(stage2.size()*block_size*sizeof(value_type))
        << std::endl;

    out
        << "\n"
        << "    static const ::boost::uint8_t " << name << "_stage1[] = {\n"
        << "\n"
        ;

    print_table(out, stage1, false, 3);
    char const* int_name = get_int_type_name(sizeof(value_type));

    out
        << "\n"
        << "    };"
        << "\n"
        << "\n"
        << "    static const " << int_name << ' ' << name << "_stage2[] = {"
        ;

    int block_n = 0;
    for (int i = 0; i < int(stage2.size()); ++i)
    {
        value_type const* p = stage2[i];
        bool last = (i+1 == stage2.size());
        out << "\n\n    // block " << block_n++ << std::endl;
        print_table(out,
            boost::iterator_range<value_type const*>(p, p+block_size), !last, field_width);
    }

    out
        << "\n"
        << "    };"
        << "\n"
        ;

    out
        << "\n"
        << "    inline " << int_name << ' ' << name << "_lookup(::boost::uint32_t ch)\n"
        << "    {\n"
        << "        ::boost::uint32_t block_offset = " << name << "_stage1[ch / " << block_size << "] * " << block_size << ";\n"
        << "        return " << name << "_stage2[block_offset + ch % " << block_size << "];\n"
        << "    }\n"
        ;

    print_tail(out);
}

int main()
{
    // The category tables
    {
        std::ofstream out("category_table.hpp");
        ucd_table_builder<uint16_t, 256> builder(ucd_info::map_property("Cn"));
        builder.collect("UnicodeData.txt", 2, assign_property);
        builder.collect("DerivedCoreProperties.txt", 1, append_property);
        builder.collect("PropList.txt", 1, append_property);
        print_file(out, builder, 4, "category");
    }

    // The script tables
    {
        std::ofstream out("script_table.hpp");
        ucd_table_builder<uint8_t, 256> builder(ucd_info::map_property("Unknown"));
        builder.collect("Scripts.txt", 1, assign_property);
        print_file(out, builder, 3, "script");
    }

    // The lowercase tables
    {
        std::ofstream out("lowercase_table.hpp");
        ucd_table_builder<uint32_t, 256> builder;
        builder.collect("UnicodeData.txt", 13, assign_code_value);
        print_file(out, builder, 6, "lowercase");
    }

    // The uppercase tables
    {
        std::ofstream out("uppercase_table.hpp");
        ucd_table_builder<uint32_t, 256> builder;
        builder.collect("UnicodeData.txt", 12, assign_code_value);
        print_file(out, builder, 6, "uppercase");
    }

    return 0;
}
