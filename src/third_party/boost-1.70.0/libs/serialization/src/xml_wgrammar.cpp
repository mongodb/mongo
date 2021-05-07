/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// xml_wgrammar.cpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/config.hpp>

#ifdef BOOST_NO_STD_WSTREAMBUF
#error "wide char i/o not supported on this platform"
#else

#define BOOST_WARCHIVE_SOURCE
#include <boost/serialization/config.hpp>
#include <boost/archive/impl/basic_xml_grammar.hpp>

using namespace boost::spirit::classic;

// fixup for RogueWave
#if ! defined(__SGI_STL_PORT) \
&& defined(BOOST_RWSTD_VER) && BOOST_RWSTD_VER<=0x020101
#include <string>
namespace std {
    template<>
    inline wstring & 
    wstring::replace (
        wchar_t * first1, 
        wchar_t * last1,
        const wchar_t * first2,
        const wchar_t * last2
    ){
        replace(first1-begin(),last1-first1,first2,last2-first2,0,last2-first2);
        return *this;
    }
} // namespace std
#endif

namespace boost {
namespace archive {

typedef basic_xml_grammar<wchar_t> xml_wgrammar;

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// specific definitions for wchar_t based XML

template<>
void xml_wgrammar::init_chset(){
    Char = chset_t(
        #if defined(__GNUC__) && defined(linux)
            L"\x9\xA\xD\x20-\xD7FF\xE000-\xFFFD\x10000-\x10FFFF"
        #else
            L"\x9\xA\xD\x20-\xD7FF\xE000-\xFFFD"
        #endif
    );

    Sch = chset_t(L"\x20\x9\xD\xA");

    BaseChar = chset_t(
        L"\x41-\x5A\x61-\x7A\xC0-\xD6\xD8-\xF6\xF8-\xFF\x100-\x131\x134-\x13E"
        L"\x141-\x148\x14A-\x17E\x180-\x1C3\x1CD-\x1F0\x1F4-\x1F5\x1FA-\x217"
        L"\x250-\x2A8\x2BB-\x2C1\x386\x388-\x38A\x38C\x38E-\x3A1\x3A3-\x3CE"
        L"\x3D0-\x3D6\x3DA\x3DC\x3DE\x3E0\x3E2-\x3F3\x401-\x40C\x40E-\x44F"
        L"\x451-\x45C\x45E-\x481\x490-\x4C4\x4C7-\x4C8\x4CB-\x4CC\x4D0-\x4EB"
        L"\x4EE-\x4F5\x4F8-\x4F9\x531-\x556\x559\x561-\x586\x5D0-\x5EA"
        L"\x5F0-\x5F2\x621-\x63A\x641-\x64A\x671-\x6B7\x6BA-\x6BE\x6C0-\x6CE"
        L"\x6D0-\x6D3\x6D5\x6E5-\x6E6\x905-\x939\x93D\x958-\x961\x985-\x98C"
        L"\x98F-\x990\x993-\x9A8\x9AA-\x9B0\x9B2\x9B6-\x9B9\x9DC-\x9DD"
        L"\x9DF-\x9E1\x9F0-\x9F1\xA05-\xA0A\xA0F-\xA10\xA13-\xA28\xA2A-\xA30"
        L"\xA32-\xA33\xA35-\xA36\xA38-\xA39\xA59-\xA5C\xA5E\xA72-\xA74"
        L"\xA85-\xA8B\xA8D\xA8F-\xA91\xA93-\xAA8\xAAA-\xAB0\xAB2-\xAB3"
        L"\xAB5-\xAB9\xABD\xAE0\xB05-\xB0C\xB0F-\xB10\xB13-\xB28\xB2A-\xB30"
        L"\xB32-\xB33\xB36-\xB39\xB3D\xB5C-\xB5D\xB5F-\xB61\xB85-\xB8A"
        L"\xB8E-\xB90\xB92-\xB95\xB99-\xB9A\xB9C\xB9E-\xB9F\xBA3-\xBA4"
        L"\xBA8-\xBAA\xBAE-\xBB5\xBB7-\xBB9\xC05-\xC0C\xC0E-\xC10\xC12-\xC28"
        L"\xC2A-\xC33\xC35-\xC39\xC60-\xC61\xC85-\xC8C\xC8E-\xC90\xC92-\xCA8"
        L"\xCAA-\xCB3\xCB5-\xCB9\xCDE\xCE0-\xCE1\xD05-\xD0C\xD0E-\xD10"
        L"\xD12-\xD28\xD2A-\xD39\xD60-\xD61\xE01-\xE2E\xE30\xE32-\xE33"
        L"\xE40-\xE45\xE81-\xE82\xE84\xE87-\xE88\xE8A\xE8D\xE94-\xE97"
        L"\xE99-\xE9F\xEA1-\xEA3\xEA5\xEA7\xEAA-\xEAB\xEAD-\xEAE\xEB0"
        L"\xEB2-\xEB3\xEBD\xEC0-\xEC4\xF40-\xF47\xF49-\xF69\x10A0-\x10C5"
        L"\x10D0-\x10F6\x1100\x1102-\x1103\x1105-\x1107\x1109\x110B-\x110C"
        L"\x110E-\x1112\x113C\x113E\x1140\x114C\x114E\x1150\x1154-\x1155"
        L"\x1159\x115F-\x1161\x1163\x1165\x1167\x1169\x116D-\x116E"
        L"\x1172-\x1173\x1175\x119E\x11A8\x11AB\x11AE-\x11AF\x11B7-\x11B8"
        L"\x11BA\x11BC-\x11C2\x11EB\x11F0\x11F9\x1E00-\x1E9B\x1EA0-\x1EF9"
        L"\x1F00-\x1F15\x1F18-\x1F1D\x1F20-\x1F45\x1F48-\x1F4D\x1F50-\x1F57"
        L"\x1F59\x1F5B\x1F5D\x1F5F-\x1F7D\x1F80-\x1FB4\x1FB6-\x1FBC\x1FBE"
        L"\x1FC2-\x1FC4\x1FC6-\x1FCC\x1FD0-\x1FD3\x1FD6-\x1FDB\x1FE0-\x1FEC"
        L"\x1FF2-\x1FF4\x1FF6-\x1FFC\x2126\x212A-\x212B\x212E\x2180-\x2182"
        L"\x3041-\x3094\x30A1-\x30FA\x3105-\x312C\xAC00-\xD7A3"
    );

    Ideographic = chset_t(L"\x4E00-\x9FA5\x3007\x3021-\x3029");

    Letter = BaseChar | Ideographic;

    CombiningChar = chset_t(
        L"\x0300-\x0345\x0360-\x0361\x0483-\x0486\x0591-\x05A1\x05A3-\x05B9"
        L"\x05BB-\x05BD\x05BF\x05C1-\x05C2\x05C4\x064B-\x0652\x0670"
        L"\x06D6-\x06DC\x06DD-\x06DF\x06E0-\x06E4\x06E7-\x06E8\x06EA-\x06ED"
        L"\x0901-\x0903\x093C\x093E-\x094C\x094D\x0951-\x0954\x0962-\x0963"
        L"\x0981-\x0983\x09BC\x09BE\x09BF\x09C0-\x09C4\x09C7-\x09C8"
        L"\x09CB-\x09CD\x09D7\x09E2-\x09E3\x0A02\x0A3C\x0A3E\x0A3F"
        L"\x0A40-\x0A42\x0A47-\x0A48\x0A4B-\x0A4D\x0A70-\x0A71\x0A81-\x0A83"
        L"\x0ABC\x0ABE-\x0AC5\x0AC7-\x0AC9\x0ACB-\x0ACD\x0B01-\x0B03\x0B3C"
        L"\x0B3E-\x0B43\x0B47-\x0B48\x0B4B-\x0B4D\x0B56-\x0B57\x0B82-\x0B83"
        L"\x0BBE-\x0BC2\x0BC6-\x0BC8\x0BCA-\x0BCD\x0BD7\x0C01-\x0C03"
        L"\x0C3E-\x0C44\x0C46-\x0C48\x0C4A-\x0C4D\x0C55-\x0C56\x0C82-\x0C83"
        L"\x0CBE-\x0CC4\x0CC6-\x0CC8\x0CCA-\x0CCD\x0CD5-\x0CD6\x0D02-\x0D03"
        L"\x0D3E-\x0D43\x0D46-\x0D48\x0D4A-\x0D4D\x0D57\x0E31\x0E34-\x0E3A"
        L"\x0E47-\x0E4E\x0EB1\x0EB4-\x0EB9\x0EBB-\x0EBC\x0EC8-\x0ECD"
        L"\x0F18-\x0F19\x0F35\x0F37\x0F39\x0F3E\x0F3F\x0F71-\x0F84"
        L"\x0F86-\x0F8B\x0F90-\x0F95\x0F97\x0F99-\x0FAD\x0FB1-\x0FB7\x0FB9"
        L"\x20D0-\x20DC\x20E1\x302A-\x302F\x3099\x309A"
    );

    Digit = chset_t(
        L"\x0030-\x0039\x0660-\x0669\x06F0-\x06F9\x0966-\x096F\x09E6-\x09EF"
        L"\x0A66-\x0A6F\x0AE6-\x0AEF\x0B66-\x0B6F\x0BE7-\x0BEF\x0C66-\x0C6F"
        L"\x0CE6-\x0CEF\x0D66-\x0D6F\x0E50-\x0E59\x0ED0-\x0ED9\x0F20-\x0F29"
    );

    Extender = chset_t(
        L"\x00B7\x02D0\x02D1\x0387\x0640\x0E46\x0EC6\x3005\x3031-\x3035"
        L"\x309D-\x309E\x30FC-\x30FE"
    );

    NameChar =
        Letter 
        | Digit 
        | L'.'
        | L'-'
        | L'_'
        | L':'
        | CombiningChar 
        | Extender
    ;
}
} // namespace archive
} // namespace boost

#include "basic_xml_grammar.ipp"

namespace boost {
namespace archive {

// explicit instantiation of xml for wide characters
template class basic_xml_grammar<wchar_t>;

} // namespace archive
} // namespace boost

#endif
