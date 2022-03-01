/*
Copyright (c) 1999 Samphan Raruenrom <samphan@thai.com>
Permission to use, copy, modify, distribute and sell this software
and its documentation for any purpose is hereby granted without fee,
provided that the above copyright notice appear in all copies and
that both that copyright notice and this permission notice appear
in supporting documentation.  Samphan Raruenrom makes no
representations about the suitability of this software for any
purpose.  It is provided "as is" without express or implied warranty.
*/
#ifndef __TH_CHAR_H__
#define __TH_CHAR_H__

typedef unsigned char tis_char;

#ifdef TH_UNICODE
/*
 * The char16_t type is only usable in C++ code, so we need this ugly hack to
 * select a binary compatible C type for the expat C code to use.
 */
#  ifdef __cplusplus
typedef char16_t th_char;
#  else
typedef uint16_t th_char;
#  endif
#  define TH_THAIBEGIN_ 0x0e00
#  define th_isthai(c) (0x0e00 <= (c) && (c) <= 0x0e5f)
#else
typedef tis_char th_char;
#  define TH_THAIBEGIN_ 0xa0
#  define th_isthai(c) ((c) >= 0xa0)
#endif
#define th_zcode(c) ((c)-TH_THAIBEGIN_)

enum TH_CHARNAME {
  TH_THAIBEGIN = TH_THAIBEGIN_,
  TH_KOKAI,
  TH_KHOKHAI,
  TH_KHOKHUAT,
  TH_KHOKHWAI,
  TH_KHOKHON,
  TH_KHORAKHANG,
  TH_NGONGU,
  TH_CHOCHAN,
  TH_CHOCHING,
  TH_CHOCHANG,
  TH_SOSO,
  TH_CHOCHOE,
  TH_YOYING,
  TH_DOCHADA,
  TH_TOPATAK,
  TH_THOTHAN,
  TH_THONANGMONTHO,
  TH_THOPHUTHAO,
  TH_NONEN,
  TH_DODEK,
  TH_TOTAO,
  TH_THOTHUNG,
  TH_THOTHAHAN,
  TH_THOTHONG,
  TH_NONU,
  TH_BOBAIMAI,
  TH_POPLA,
  TH_PHOPHUNG,
  TH_FOFA,
  TH_PHOPHAN,
  TH_FOFAN,
  TH_PHOSAMPHAO,
  TH_MOMA,
  TH_YOYAK,
  TH_RORUA,
  TH_RU,
  TH_LOLING,
  TH_LU,
  TH_WOWAEN,
  TH_SOSALA,
  TH_SORUSI,
  TH_SOSUA,
  TH_HOHIP,
  TH_LOCHULA,
  TH_OANG,
  TH_HONOKHUK,
  TH_PAIYANNOI,
  TH_SARA_A,
  TH_MAIHANAKAT,
  TH_SARA_AA,
  TH_SARA_AM,
  TH_SARA_I,
  TH_SARA_II,
  TH_SARA_UE,
  TH_SARA_UEE,
  TH_SARA_U,
  TH_SARA_UU,
  TH_PHINTHU,
  TH_REM_CHERNG_,
  TH_TAC_WBRK_,
  TH_UNDEF_DD,
  TH_UNDEF_DE,
  TH_BAHT,
  TH_SARA_E,
  TH_SARA_AE,
  TH_SARA_O,
  TH_MAIMUAN,
  TH_MAIMALAI,
  TH_LAKKHANGYAO,
  TH_MAIYAMOK,
  TH_MAITAIKHU,
  TH_MAIEK,
  TH_MAITHO,
  TH_MAITRI,
  TH_MAICHATTAWA,
  TH_THANTHAKHAT,
  TH_NIKHAHIT,
  TH_YAMAKKAN,
  TH_FONGMAN,
  TH_THAIZERO,
  TH_THAIONE,
  TH_THAITWO,
  TH_THAITHREE,
  TH_THAIFOUR,
  TH_THAIFIVE,
  TH_THAISIX,
  TH_THAISEVEN,
  TH_THAIEIGHT,
  TH_THAININE,
  TH_ANGKHANKHU,
  TH_KHOMUT,
  TH_UNDEF_FC,
  TH_UNDEF_FD,
  TH_UNDEF_FE,
  TH_THAIEND
};
#endif
