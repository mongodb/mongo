
/* This file was generated automatically by the Snowball to ANSI C compiler */

#include "../runtime/header.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int slovak_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif
static int r_end_vowel(struct SN_env * z);
static int r_suffixes(struct SN_env * z);
static int r_prefixes(struct SN_env * z);
static int r_mark_p2(struct SN_env * z);
static int r_mark_p1(struct SN_env * z);
static int r_exception(struct SN_env * z);
static int r_un_accent(struct SN_env * z);
static int r_lower_case(struct SN_env * z);
static int r_R2(struct SN_env * z);
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * slovak_UTF_8_create_env(void);
extern void slovak_UTF_8_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_1[3] = { 'd', 0xC5, 0xBE };
static const symbol s_0_2[2] = { 0xC3, 0xA1 };
static const symbol s_0_3[2] = { 0xC3, 0xA4 };
static const symbol s_0_4[2] = { 0xC3, 0xA9 };
static const symbol s_0_5[2] = { 0xC3, 0xAD };
static const symbol s_0_6[2] = { 0xC3, 0xB3 };
static const symbol s_0_7[2] = { 0xC3, 0xB4 };
static const symbol s_0_8[2] = { 0xC3, 0xBA };
static const symbol s_0_9[2] = { 0xC3, 0xBD };
static const symbol s_0_10[2] = { 0xC4, 0x8D };
static const symbol s_0_11[2] = { 0xC4, 0x8F };
static const symbol s_0_12[2] = { 0xC4, 0x9B };
static const symbol s_0_13[2] = { 0xC4, 0xBA };
static const symbol s_0_14[2] = { 0xC4, 0xBE };
static const symbol s_0_15[2] = { 0xC5, 0x88 };
static const symbol s_0_16[2] = { 0xC5, 0x95 };
static const symbol s_0_17[2] = { 0xC5, 0x99 };
static const symbol s_0_18[2] = { 0xC5, 0xA1 };
static const symbol s_0_19[2] = { 0xC5, 0xA5 };
static const symbol s_0_20[2] = { 0xC5, 0xAF };
static const symbol s_0_21[2] = { 0xC5, 0xBE };

static const struct among a_0[22] =
{
/*  0 */ { 0, 0, -1, 22, 0},
/*  1 */ { 3, s_0_1, 0, 5, 0},
/*  2 */ { 2, s_0_2, 0, 1, 0},
/*  3 */ { 2, s_0_3, 0, 2, 0},
/*  4 */ { 2, s_0_4, 0, 6, 0},
/*  5 */ { 2, s_0_5, 0, 8, 0},
/*  6 */ { 2, s_0_6, 0, 13, 0},
/*  7 */ { 2, s_0_7, 0, 12, 0},
/*  8 */ { 2, s_0_8, 0, 18, 0},
/*  9 */ { 2, s_0_9, 0, 20, 0},
/* 10 */ { 2, s_0_10, 0, 3, 0},
/* 11 */ { 2, s_0_11, 0, 4, 0},
/* 12 */ { 2, s_0_12, 0, 7, 0},
/* 13 */ { 2, s_0_13, 0, 9, 0},
/* 14 */ { 2, s_0_14, 0, 10, 0},
/* 15 */ { 2, s_0_15, 0, 11, 0},
/* 16 */ { 2, s_0_16, 0, 14, 0},
/* 17 */ { 2, s_0_17, 0, 15, 0},
/* 18 */ { 2, s_0_18, 0, 16, 0},
/* 19 */ { 2, s_0_19, 0, 17, 0},
/* 20 */ { 2, s_0_20, 0, 19, 0},
/* 21 */ { 2, s_0_21, 0, 21, 0}
};

static const symbol s_1_1[3] = { 'd', 0xC5, 0xBD };
static const symbol s_1_2[2] = { 0xC3, 0x81 };
static const symbol s_1_3[2] = { 0xC3, 0x84 };
static const symbol s_1_4[2] = { 0xC3, 0x89 };
static const symbol s_1_5[2] = { 0xC3, 0x8D };
static const symbol s_1_6[2] = { 0xC3, 0x93 };
static const symbol s_1_7[2] = { 0xC3, 0x94 };
static const symbol s_1_8[2] = { 0xC3, 0x9A };
static const symbol s_1_9[2] = { 0xC3, 0x9D };
static const symbol s_1_10[2] = { 0xC4, 0x8C };
static const symbol s_1_11[2] = { 0xC4, 0x8E };
static const symbol s_1_12[2] = { 0xC4, 0x9A };
static const symbol s_1_13[2] = { 0xC4, 0xB9 };
static const symbol s_1_14[2] = { 0xC4, 0xBD };
static const symbol s_1_15[2] = { 0xC4, 0xBE };
static const symbol s_1_16[2] = { 0xC5, 0x94 };
static const symbol s_1_17[2] = { 0xC5, 0x98 };
static const symbol s_1_18[2] = { 0xC5, 0xA0 };
static const symbol s_1_19[2] = { 0xC5, 0xA4 };
static const symbol s_1_20[2] = { 0xC5, 0xAE };
static const symbol s_1_21[2] = { 0xC5, 0xBD };

static const struct among a_1[22] =
{
/*  0 */ { 0, 0, -1, 22, 0},
/*  1 */ { 3, s_1_1, 0, 5, 0},
/*  2 */ { 2, s_1_2, 0, 1, 0},
/*  3 */ { 2, s_1_3, 0, 2, 0},
/*  4 */ { 2, s_1_4, 0, 6, 0},
/*  5 */ { 2, s_1_5, 0, 8, 0},
/*  6 */ { 2, s_1_6, 0, 13, 0},
/*  7 */ { 2, s_1_7, 0, 12, 0},
/*  8 */ { 2, s_1_8, 0, 18, 0},
/*  9 */ { 2, s_1_9, 0, 20, 0},
/* 10 */ { 2, s_1_10, 0, 3, 0},
/* 11 */ { 2, s_1_11, 0, 4, 0},
/* 12 */ { 2, s_1_12, 0, 7, 0},
/* 13 */ { 2, s_1_13, 0, 9, 0},
/* 14 */ { 2, s_1_14, 0, 11, 0},
/* 15 */ { 2, s_1_15, 0, 10, 0},
/* 16 */ { 2, s_1_16, 0, 14, 0},
/* 17 */ { 2, s_1_17, 0, 15, 0},
/* 18 */ { 2, s_1_18, 0, 16, 0},
/* 19 */ { 2, s_1_19, 0, 17, 0},
/* 20 */ { 2, s_1_20, 0, 19, 0},
/* 21 */ { 2, s_1_21, 0, 21, 0}
};

static const symbol s_2_0[7] = { 'b', 'a', 'b', 'i', 'c', 'k', 'a' };
static const symbol s_2_1[6] = { 'b', 'a', 'b', 'i', 'e', 'k' };
static const symbol s_2_2[3] = { 'b', 'o', 'l' };
static const symbol s_2_3[4] = { 'b', 'o', 'l', 'a' };
static const symbol s_2_4[4] = { 'b', 'o', 'l', 'o' };
static const symbol s_2_5[3] = { 'b', 'u', 'd' };
static const symbol s_2_6[4] = { 'b', 'u', 'd', 'e' };
static const symbol s_2_7[5] = { 'b', 'u', 'd', 'e', 'm' };
static const symbol s_2_8[6] = { 'b', 'u', 'd', 'e', 'm', 'e' };
static const symbol s_2_9[5] = { 'b', 'u', 'd', 'e', 's' };
static const symbol s_2_10[6] = { 'b', 'u', 'd', 'e', 't', 'e' };
static const symbol s_2_11[5] = { 'b', 'u', 'd', 'm', 'e' };
static const symbol s_2_12[5] = { 'b', 'u', 'd', 't', 'e' };
static const symbol s_2_13[4] = { 'b', 'u', 'd', 'u' };
static const symbol s_2_14[4] = { 'c', 'h', 'o', 'd' };
static const symbol s_2_15[6] = { 'c', 'h', 'o', 'd', 't', 'e' };
static const symbol s_2_16[4] = { 'd', 'l', 'a', 'n' };
static const symbol s_2_17[3] = { 'i', 'd', 'e' };
static const symbol s_2_18[4] = { 'i', 'd', 'e', 'm' };
static const symbol s_2_19[5] = { 'i', 'd', 'e', 'm', 'e' };
static const symbol s_2_20[4] = { 'i', 'd', 'e', 's' };
static const symbol s_2_21[5] = { 'i', 'd', 'e', 't', 'e' };
static const symbol s_2_22[3] = { 'i', 'd', 'u' };
static const symbol s_2_23[4] = { 'i', 'd', 'u', 'c' };
static const symbol s_2_24[5] = { 'i', 'd', 'u', 'c', 'a' };
static const symbol s_2_25[5] = { 'i', 'd', 'u', 'c', 'e' };
static const symbol s_2_26[5] = { 'i', 'd', 'u', 'c', 'i' };
static const symbol s_2_27[5] = { 'i', 's', 'i', 'e', 'l' };
static const symbol s_2_28[4] = { 'i', 's', 'l', 'a' };
static const symbol s_2_29[4] = { 'i', 's', 'l', 'i' };
static const symbol s_2_30[4] = { 'i', 's', 'l', 'o' };
static const symbol s_2_31[2] = { 'j', 'e' };
static const symbol s_2_32[5] = { 'j', 'e', 'd', 'i', 'a' };
static const symbol s_2_33[5] = { 'j', 'e', 'd', 'l', 'a' };
static const symbol s_2_34[5] = { 'j', 'e', 'd', 'l', 'i' };
static const symbol s_2_35[5] = { 'j', 'e', 'd', 'l', 'o' };
static const symbol s_2_36[5] = { 'j', 'e', 'd', 'o', 'l' };
static const symbol s_2_37[4] = { 'j', 'e', 'd', 'z' };
static const symbol s_2_38[6] = { 'j', 'e', 'd', 'z', 'm', 'e' };
static const symbol s_2_39[6] = { 'j', 'e', 'd', 'z', 't', 'e' };
static const symbol s_2_40[3] = { 'j', 'e', 'm' };
static const symbol s_2_41[4] = { 'j', 'e', 'm', 'e' };
static const symbol s_2_42[3] = { 'j', 'e', 's' };
static const symbol s_2_43[4] = { 'j', 'e', 't', 'e' };
static const symbol s_2_44[2] = { 'm', 'a' };
static const symbol s_2_45[3] = { 'm', 'a', 'j' };
static const symbol s_2_46[7] = { 'm', 'a', 'j', 'e', 't', 'o', 'k' };
static const symbol s_2_47[5] = { 'm', 'a', 'j', 'm', 'e' };
static const symbol s_2_48[5] = { 'm', 'a', 'j', 't', 'e' };
static const symbol s_2_49[4] = { 'm', 'a', 'j', 'u' };
static const symbol s_2_50[3] = { 'm', 'a', 'l' };
static const symbol s_2_51[4] = { 'm', 'a', 'l', 'a' };
static const symbol s_2_52[4] = { 'm', 'a', 'l', 'i' };
static const symbol s_2_53[4] = { 'm', 'a', 'l', 'o' };
static const symbol s_2_54[3] = { 'm', 'a', 'm' };
static const symbol s_2_55[4] = { 'm', 'a', 'm', 'e' };
static const symbol s_2_56[7] = { 'm', 'a', 'm', 'i', 'c', 'k', 'a' };
static const symbol s_2_57[5] = { 'm', 'a', 'm', 'k', 'a' };
static const symbol s_2_58[3] = { 'm', 'a', 's' };
static const symbol s_2_59[4] = { 'm', 'a', 't', 'e' };
static const symbol s_2_60[7] = { 'm', 'a', 't', 'i', 'c', 'k', 'a' };
static const symbol s_2_61[5] = { 'm', 'a', 't', 'k', 'a' };
static const symbol s_2_62[5] = { 'm', 'e', 's', 't', 'e' };
static const symbol s_2_63[5] = { 'm', 'i', 'e', 's', 't' };
static const symbol s_2_64[3] = { 'p', 'o', 'd' };
static const symbol s_2_65[5] = { 'p', 'o', 'd', 'm', 'e' };
static const symbol s_2_66[5] = { 'p', 'o', 'd', 't', 'e' };
static const symbol s_2_67[5] = { 'p', 'o', 'j', 'd', 'e' };
static const symbol s_2_68[6] = { 'p', 'o', 'j', 'd', 'e', 'm' };
static const symbol s_2_69[7] = { 'p', 'o', 'j', 'd', 'e', 'm', 'e' };
static const symbol s_2_70[6] = { 'p', 'o', 'j', 'd', 'e', 's' };
static const symbol s_2_71[7] = { 'p', 'o', 'j', 'd', 'e', 't', 'e' };
static const symbol s_2_72[5] = { 'p', 'o', 'j', 'd', 'u' };
static const symbol s_2_73[2] = { 's', 'i' };
static const symbol s_2_74[4] = { 's', 'i', 'e', 'l' };
static const symbol s_2_75[3] = { 's', 'l', 'a' };
static const symbol s_2_76[3] = { 's', 'l', 'i' };
static const symbol s_2_77[3] = { 's', 'l', 'o' };
static const symbol s_2_78[3] = { 's', 'm', 'e' };
static const symbol s_2_79[3] = { 's', 'o', 'm' };
static const symbol s_2_80[4] = { 's', 'r', 'a', 'l' };
static const symbol s_2_81[5] = { 's', 'r', 'a', 'l', 'i' };
static const symbol s_2_82[3] = { 's', 't', 'e' };
static const symbol s_2_83[2] = { 's', 'u' };
static const symbol s_2_84[4] = { 'z', 'i', 'e', 'n' };

static const struct among a_2[85] =
{
/*  0 */ { 7, s_2_0, -1, 5, 0},
/*  1 */ { 6, s_2_1, -1, 5, 0},
/*  2 */ { 3, s_2_2, -1, 1, 0},
/*  3 */ { 4, s_2_3, 2, 1, 0},
/*  4 */ { 4, s_2_4, 2, 1, 0},
/*  5 */ { 3, s_2_5, -1, 1, 0},
/*  6 */ { 4, s_2_6, 5, 1, 0},
/*  7 */ { 5, s_2_7, 6, 1, 0},
/*  8 */ { 6, s_2_8, 7, 1, 0},
/*  9 */ { 5, s_2_9, 6, 1, 0},
/* 10 */ { 6, s_2_10, 6, 1, 0},
/* 11 */ { 5, s_2_11, 5, 1, 0},
/* 12 */ { 5, s_2_12, 5, 1, 0},
/* 13 */ { 4, s_2_13, 5, 1, 0},
/* 14 */ { 4, s_2_14, -1, 3, 0},
/* 15 */ { 6, s_2_15, 14, 3, 0},
/* 16 */ { 4, s_2_16, -1, 6, 0},
/* 17 */ { 3, s_2_17, -1, 3, 0},
/* 18 */ { 4, s_2_18, 17, 3, 0},
/* 19 */ { 5, s_2_19, 18, 3, 0},
/* 20 */ { 4, s_2_20, 17, 3, 0},
/* 21 */ { 5, s_2_21, 17, 3, 0},
/* 22 */ { 3, s_2_22, -1, 3, 0},
/* 23 */ { 4, s_2_23, 22, 3, 0},
/* 24 */ { 5, s_2_24, 23, 3, 0},
/* 25 */ { 5, s_2_25, 23, 3, 0},
/* 26 */ { 5, s_2_26, 23, 3, 0},
/* 27 */ { 5, s_2_27, -1, 3, 0},
/* 28 */ { 4, s_2_28, -1, 3, 0},
/* 29 */ { 4, s_2_29, -1, 3, 0},
/* 30 */ { 4, s_2_30, -1, 3, 0},
/* 31 */ { 2, s_2_31, -1, 1, 0},
/* 32 */ { 5, s_2_32, 31, 4, 0},
/* 33 */ { 5, s_2_33, 31, 4, 0},
/* 34 */ { 5, s_2_34, 31, 4, 0},
/* 35 */ { 5, s_2_35, 31, 4, 0},
/* 36 */ { 5, s_2_36, 31, 4, 0},
/* 37 */ { 4, s_2_37, 31, 4, 0},
/* 38 */ { 6, s_2_38, 37, 4, 0},
/* 39 */ { 6, s_2_39, 37, 4, 0},
/* 40 */ { 3, s_2_40, 31, 4, 0},
/* 41 */ { 4, s_2_41, 40, 4, 0},
/* 42 */ { 3, s_2_42, 31, 4, 0},
/* 43 */ { 4, s_2_43, 31, 4, 0},
/* 44 */ { 2, s_2_44, -1, 2, 0},
/* 45 */ { 3, s_2_45, 44, 2, 0},
/* 46 */ { 7, s_2_46, 45, 8, 0},
/* 47 */ { 5, s_2_47, 45, 2, 0},
/* 48 */ { 5, s_2_48, 45, 2, 0},
/* 49 */ { 4, s_2_49, 45, 2, 0},
/* 50 */ { 3, s_2_50, 44, 2, 0},
/* 51 */ { 4, s_2_51, 50, 2, 0},
/* 52 */ { 4, s_2_52, 50, 2, 0},
/* 53 */ { 4, s_2_53, 50, 2, 0},
/* 54 */ { 3, s_2_54, 44, 2, 0},
/* 55 */ { 4, s_2_55, 54, 2, 0},
/* 56 */ { 7, s_2_56, 54, 7, 0},
/* 57 */ { 5, s_2_57, 54, 7, 0},
/* 58 */ { 3, s_2_58, 44, 2, 0},
/* 59 */ { 4, s_2_59, 44, 2, 0},
/* 60 */ { 7, s_2_60, 44, 7, 0},
/* 61 */ { 5, s_2_61, 44, 7, 0},
/* 62 */ { 5, s_2_62, -1, 9, 0},
/* 63 */ { 5, s_2_63, -1, 9, 0},
/* 64 */ { 3, s_2_64, -1, 3, 0},
/* 65 */ { 5, s_2_65, 64, 3, 0},
/* 66 */ { 5, s_2_66, 64, 3, 0},
/* 67 */ { 5, s_2_67, -1, 3, 0},
/* 68 */ { 6, s_2_68, 67, 3, 0},
/* 69 */ { 7, s_2_69, 68, 3, 0},
/* 70 */ { 6, s_2_70, 67, 3, 0},
/* 71 */ { 7, s_2_71, 67, 3, 0},
/* 72 */ { 5, s_2_72, -1, 3, 0},
/* 73 */ { 2, s_2_73, -1, 1, 0},
/* 74 */ { 4, s_2_74, 73, 3, 0},
/* 75 */ { 3, s_2_75, -1, 3, 0},
/* 76 */ { 3, s_2_76, -1, 3, 0},
/* 77 */ { 3, s_2_77, -1, 3, 0},
/* 78 */ { 3, s_2_78, -1, 1, 0},
/* 79 */ { 3, s_2_79, -1, 1, 0},
/* 80 */ { 4, s_2_80, -1, 10, 0},
/* 81 */ { 5, s_2_81, 80, 10, 0},
/* 82 */ { 3, s_2_82, -1, 1, 0},
/* 83 */ { 2, s_2_83, -1, 1, 0},
/* 84 */ { 4, s_2_84, -1, 11, 0}
};

static const symbol s_3_0[3] = { 'b', 'e', 'z' };
static const symbol s_3_1[3] = { 'c', 'e', 'z' };
static const symbol s_3_2[2] = { 'd', 'o' };
static const symbol s_3_3[3] = { 'n', 'a', 'd' };
static const symbol s_3_4[3] = { 'n', 'a', 'j' };
static const symbol s_3_5[2] = { 'n', 'e' };
static const symbol s_3_6[2] = { 'o', 'b' };
static const symbol s_3_7[2] = { 'o', 'd' };
static const symbol s_3_8[2] = { 'p', 'o' };
static const symbol s_3_9[3] = { 'p', 'o', 'd' };
static const symbol s_3_10[3] = { 'p', 'r', 'e' };
static const symbol s_3_11[3] = { 'p', 'r', 'i' };
static const symbol s_3_12[5] = { 'p', 'r', 'o', 't', 'i' };
static const symbol s_3_13[3] = { 'r', 'o', 'z' };
static const symbol s_3_14[2] = { 'v', 'y' };
static const symbol s_3_15[2] = { 'v', 'z' };
static const symbol s_3_16[2] = { 'z', 'a' };

static const struct among a_3[17] =
{
/*  0 */ { 3, s_3_0, -1, 2, 0},
/*  1 */ { 3, s_3_1, -1, 2, 0},
/*  2 */ { 2, s_3_2, -1, 3, 0},
/*  3 */ { 3, s_3_3, -1, 2, 0},
/*  4 */ { 3, s_3_4, -1, 2, 0},
/*  5 */ { 2, s_3_5, -1, 3, 0},
/*  6 */ { 2, s_3_6, -1, 3, 0},
/*  7 */ { 2, s_3_7, -1, 3, 0},
/*  8 */ { 2, s_3_8, -1, 3, 0},
/*  9 */ { 3, s_3_9, 8, 2, 0},
/* 10 */ { 3, s_3_10, -1, 2, 0},
/* 11 */ { 3, s_3_11, -1, 2, 0},
/* 12 */ { 5, s_3_12, -1, 1, 0},
/* 13 */ { 3, s_3_13, -1, 2, 0},
/* 14 */ { 2, s_3_14, -1, 3, 0},
/* 15 */ { 2, s_3_15, -1, 3, 0},
/* 16 */ { 2, s_3_16, -1, 3, 0}
};

static const symbol s_4_0[1] = { 'a' };
static const symbol s_4_1[2] = { 'i', 'a' };
static const symbol s_4_2[5] = { 'e', 'j', 's', 'i', 'a' };
static const symbol s_4_3[4] = { 'o', 'v', 'i', 'a' };
static const symbol s_4_4[6] = { 'i', 'n', 'o', 'v', 'i', 'a' };
static const symbol s_4_5[3] = { 's', 'k', 'a' };
static const symbol s_4_6[3] = { 'a', 'l', 'a' };
static const symbol s_4_7[5] = { 'o', 'v', 'a', 'l', 'a' };
static const symbol s_4_8[3] = { 'i', 'l', 'a' };
static const symbol s_4_9[3] = { 'i', 'n', 'a' };
static const symbol s_4_10[3] = { 'a', 't', 'a' };
static const symbol s_4_11[2] = { 'v', 'a' };
static const symbol s_4_12[5] = { 'e', 'n', 'i', 'e', 'c' };
static const symbol s_4_13[4] = { 'u', 'j', 'u', 'c' };
static const symbol s_4_14[1] = { 'e' };
static const symbol s_4_15[4] = { 'e', 'n', 'c', 'e' };
static const symbol s_4_16[2] = { 'i', 'e' };
static const symbol s_4_17[3] = { 's', 'i', 'e' };
static const symbol s_4_18[5] = { 'e', 'j', 's', 'i', 'e' };
static const symbol s_4_19[3] = { 'u', 'j', 'e' };
static const symbol s_4_20[2] = { 'm', 'e' };
static const symbol s_4_21[3] = { 'a', 'm', 'e' };
static const symbol s_4_22[5] = { 'a', 'v', 'a', 'm', 'e' };
static const symbol s_4_23[3] = { 'e', 'm', 'e' };
static const symbol s_4_24[4] = { 'i', 'e', 'm', 'e' };
static const symbol s_4_25[5] = { 'u', 'j', 'e', 'm', 'e' };
static const symbol s_4_26[3] = { 'i', 'm', 'e' };
static const symbol s_4_27[3] = { 'j', 'm', 'e' };
static const symbol s_4_28[4] = { 'u', 'j', 'm', 'e' };
static const symbol s_4_29[3] = { 'i', 'n', 'e' };
static const symbol s_4_30[2] = { 't', 'e' };
static const symbol s_4_31[3] = { 'a', 't', 'e' };
static const symbol s_4_32[5] = { 'a', 'v', 'a', 't', 'e' };
static const symbol s_4_33[3] = { 'e', 't', 'e' };
static const symbol s_4_34[4] = { 'i', 'e', 't', 'e' };
static const symbol s_4_35[5] = { 'u', 'j', 'e', 't', 'e' };
static const symbol s_4_36[3] = { 'i', 't', 'e' };
static const symbol s_4_37[3] = { 'j', 't', 'e' };
static const symbol s_4_38[4] = { 'u', 'j', 't', 'e' };
static const symbol s_4_39[2] = { 'v', 'e' };
static const symbol s_4_40[3] = { 'o', 'v', 'e' };
static const symbol s_4_41[3] = { 'a', 'c', 'h' };
static const symbol s_4_42[4] = { 'i', 'a', 'c', 'h' };
static const symbol s_4_43[5] = { 'a', 't', 'a', 'c', 'h' };
static const symbol s_4_44[3] = { 'i', 'c', 'h' };
static const symbol s_4_45[6] = { 'e', 'j', 's', 'i', 'c', 'h' };
static const symbol s_4_46[3] = { 'o', 'c', 'h' };
static const symbol s_4_47[6] = { 'e', 'n', 'c', 'o', 'c', 'h' };
static const symbol s_4_48[5] = { 'i', 'n', 'o', 'c', 'h' };
static const symbol s_4_49[3] = { 'y', 'c', 'h' };
static const symbol s_4_50[5] = { 'i', 'n', 'y', 'c', 'h' };
static const symbol s_4_51[5] = { 'o', 'v', 'y', 'c', 'h' };
static const symbol s_4_52[1] = { 'i' };
static const symbol s_4_53[3] = { 'a', 'l', 'i' };
static const symbol s_4_54[5] = { 'o', 'v', 'a', 'l', 'i' };
static const symbol s_4_55[3] = { 'i', 'l', 'i' };
static const symbol s_4_56[2] = { 'm', 'i' };
static const symbol s_4_57[3] = { 'a', 'm', 'i' };
static const symbol s_4_58[6] = { 'e', 'n', 'c', 'a', 'm', 'i' };
static const symbol s_4_59[5] = { 'i', 'n', 'a', 'm', 'i' };
static const symbol s_4_60[5] = { 'a', 't', 'a', 'm', 'i' };
static const symbol s_4_61[3] = { 'i', 'm', 'i' };
static const symbol s_4_62[6] = { 'e', 'j', 's', 'i', 'm', 'i' };
static const symbol s_4_63[3] = { 'y', 'm', 'i' };
static const symbol s_4_64[5] = { 'i', 'n', 'y', 'm', 'i' };
static const symbol s_4_65[5] = { 'o', 'v', 'y', 'm', 'i' };
static const symbol s_4_66[3] = { 'i', 'n', 'i' };
static const symbol s_4_67[4] = { 'e', 'j', 's', 'i' };
static const symbol s_4_68[3] = { 'a', 't', 'i' };
static const symbol s_4_69[3] = { 'o', 'v', 'i' };
static const symbol s_4_70[5] = { 'i', 'n', 'o', 'v', 'i' };
static const symbol s_4_71[2] = { 'e', 'j' };
static const symbol s_4_72[4] = { 'i', 'n', 'e', 'j' };
static const symbol s_4_73[5] = { 'e', 'j', 's', 'e', 'j' };
static const symbol s_4_74[4] = { 'o', 'v', 'e', 'j' };
static const symbol s_4_75[2] = { 'a', 'l' };
static const symbol s_4_76[4] = { 'o', 'v', 'a', 'l' };
static const symbol s_4_77[2] = { 'i', 'l' };
static const symbol s_4_78[1] = { 'm' };
static const symbol s_4_79[2] = { 'a', 'm' };
static const symbol s_4_80[3] = { 'i', 'a', 'm' };
static const symbol s_4_81[4] = { 'a', 't', 'a', 'm' };
static const symbol s_4_82[4] = { 'a', 'v', 'a', 'm' };
static const symbol s_4_83[3] = { 'i', 'e', 'm' };
static const symbol s_4_84[4] = { 'u', 'j', 'e', 'm' };
static const symbol s_4_85[2] = { 'i', 'm' };
static const symbol s_4_86[5] = { 'e', 'j', 's', 'i', 'm' };
static const symbol s_4_87[2] = { 'o', 'm' };
static const symbol s_4_88[5] = { 'e', 'n', 'c', 'o', 'm' };
static const symbol s_4_89[4] = { 'i', 'n', 'o', 'm' };
static const symbol s_4_90[5] = { 'e', 'j', 's', 'o', 'm' };
static const symbol s_4_91[4] = { 'a', 't', 'o', 'm' };
static const symbol s_4_92[4] = { 'o', 'v', 'o', 'm' };
static const symbol s_4_93[2] = { 'y', 'm' };
static const symbol s_4_94[4] = { 'i', 'n', 'y', 'm' };
static const symbol s_4_95[4] = { 'o', 'v', 'y', 'm' };
static const symbol s_4_96[1] = { 'n' };
static const symbol s_4_97[2] = { 'i', 'n' };
static const symbol s_4_98[2] = { 'h', 'o' };
static const symbol s_4_99[3] = { 'e', 'h', 'o' };
static const symbol s_4_100[4] = { 'i', 'e', 'h', 'o' };
static const symbol s_4_101[7] = { 'e', 'j', 's', 'i', 'e', 'h', 'o' };
static const symbol s_4_102[4] = { 'i', 'n', 'h', 'o' };
static const symbol s_4_103[4] = { 'o', 'v', 'h', 'o' };
static const symbol s_4_104[3] = { 'a', 'l', 'o' };
static const symbol s_4_105[5] = { 'o', 'v', 'a', 'l', 'o' };
static const symbol s_4_106[3] = { 'i', 'l', 'o' };
static const symbol s_4_107[3] = { 'i', 'n', 'o' };
static const symbol s_4_108[2] = { 'v', 'o' };
static const symbol s_4_109[3] = { 'o', 'v', 'o' };
static const symbol s_4_110[2] = { 'a', 's' };
static const symbol s_4_111[4] = { 'a', 'v', 'a', 's' };
static const symbol s_4_112[2] = { 'e', 's' };
static const symbol s_4_113[3] = { 'i', 'e', 's' };
static const symbol s_4_114[4] = { 'u', 'j', 'e', 's' };
static const symbol s_4_115[2] = { 'i', 's' };
static const symbol s_4_116[2] = { 'a', 't' };
static const symbol s_4_117[2] = { 'i', 't' };
static const symbol s_4_118[1] = { 'u' };
static const symbol s_4_119[2] = { 'i', 'u' };
static const symbol s_4_120[5] = { 'e', 'j', 's', 'i', 'u' };
static const symbol s_4_121[3] = { 'a', 'j', 'u' };
static const symbol s_4_122[5] = { 'a', 'v', 'a', 'j', 'u' };
static const symbol s_4_123[3] = { 'u', 'j', 'u' };
static const symbol s_4_124[2] = { 'm', 'u' };
static const symbol s_4_125[3] = { 'e', 'm', 'u' };
static const symbol s_4_126[4] = { 'i', 'e', 'm', 'u' };
static const symbol s_4_127[7] = { 'e', 'j', 's', 'i', 'e', 'm', 'u' };
static const symbol s_4_128[4] = { 'i', 'n', 'm', 'u' };
static const symbol s_4_129[4] = { 'o', 'v', 'm', 'u' };
static const symbol s_4_130[3] = { 'i', 'n', 'u' };
static const symbol s_4_131[2] = { 'o', 'u' };
static const symbol s_4_132[4] = { 'i', 'n', 'o', 'u' };
static const symbol s_4_133[5] = { 'e', 'j', 's', 'o', 'u' };
static const symbol s_4_134[4] = { 'o', 'v', 'o', 'u' };
static const symbol s_4_135[3] = { 'a', 't', 'u' };
static const symbol s_4_136[2] = { 'v', 'u' };
static const symbol s_4_137[3] = { 'o', 'v', 'u' };
static const symbol s_4_138[1] = { 'v' };
static const symbol s_4_139[2] = { 'o', 'v' };
static const symbol s_4_140[4] = { 'i', 'n', 'o', 'v' };
static const symbol s_4_141[1] = { 'y' };
static const symbol s_4_142[3] = { 'o', 'v', 'y' };

static const struct among a_4[143] =
{
/*  0 */ { 1, s_4_0, -1, 1, 0},
/*  1 */ { 2, s_4_1, 0, 1, 0},
/*  2 */ { 5, s_4_2, 1, 1, 0},
/*  3 */ { 4, s_4_3, 1, 1, 0},
/*  4 */ { 6, s_4_4, 3, 1, 0},
/*  5 */ { 3, s_4_5, 0, 1, 0},
/*  6 */ { 3, s_4_6, 0, 1, 0},
/*  7 */ { 5, s_4_7, 6, 1, 0},
/*  8 */ { 3, s_4_8, 0, 1, 0},
/*  9 */ { 3, s_4_9, 0, 1, 0},
/* 10 */ { 3, s_4_10, 0, 1, 0},
/* 11 */ { 2, s_4_11, 0, 1, 0},
/* 12 */ { 5, s_4_12, -1, 1, 0},
/* 13 */ { 4, s_4_13, -1, 1, 0},
/* 14 */ { 1, s_4_14, -1, 1, 0},
/* 15 */ { 4, s_4_15, 14, 1, 0},
/* 16 */ { 2, s_4_16, 14, 1, 0},
/* 17 */ { 3, s_4_17, 16, 1, 0},
/* 18 */ { 5, s_4_18, 17, 1, 0},
/* 19 */ { 3, s_4_19, 14, 1, 0},
/* 20 */ { 2, s_4_20, 14, 1, 0},
/* 21 */ { 3, s_4_21, 20, 1, 0},
/* 22 */ { 5, s_4_22, 21, 1, 0},
/* 23 */ { 3, s_4_23, 20, 1, 0},
/* 24 */ { 4, s_4_24, 23, 1, 0},
/* 25 */ { 5, s_4_25, 23, 1, 0},
/* 26 */ { 3, s_4_26, 20, 1, 0},
/* 27 */ { 3, s_4_27, 20, 1, 0},
/* 28 */ { 4, s_4_28, 27, 1, 0},
/* 29 */ { 3, s_4_29, 14, 1, 0},
/* 30 */ { 2, s_4_30, 14, 1, 0},
/* 31 */ { 3, s_4_31, 30, 1, 0},
/* 32 */ { 5, s_4_32, 31, 1, 0},
/* 33 */ { 3, s_4_33, 30, 1, 0},
/* 34 */ { 4, s_4_34, 33, 1, 0},
/* 35 */ { 5, s_4_35, 33, 1, 0},
/* 36 */ { 3, s_4_36, 30, 1, 0},
/* 37 */ { 3, s_4_37, 30, 1, 0},
/* 38 */ { 4, s_4_38, 37, 1, 0},
/* 39 */ { 2, s_4_39, 14, 1, 0},
/* 40 */ { 3, s_4_40, 39, 1, 0},
/* 41 */ { 3, s_4_41, -1, 1, 0},
/* 42 */ { 4, s_4_42, 41, 1, 0},
/* 43 */ { 5, s_4_43, 41, 1, 0},
/* 44 */ { 3, s_4_44, -1, 1, 0},
/* 45 */ { 6, s_4_45, 44, 1, 0},
/* 46 */ { 3, s_4_46, -1, 1, 0},
/* 47 */ { 6, s_4_47, 46, 1, 0},
/* 48 */ { 5, s_4_48, 46, 1, 0},
/* 49 */ { 3, s_4_49, -1, 1, 0},
/* 50 */ { 5, s_4_50, 49, 1, 0},
/* 51 */ { 5, s_4_51, 49, 1, 0},
/* 52 */ { 1, s_4_52, -1, 1, 0},
/* 53 */ { 3, s_4_53, 52, 1, 0},
/* 54 */ { 5, s_4_54, 53, 1, 0},
/* 55 */ { 3, s_4_55, 52, 1, 0},
/* 56 */ { 2, s_4_56, 52, 1, 0},
/* 57 */ { 3, s_4_57, 56, 1, 0},
/* 58 */ { 6, s_4_58, 57, 1, 0},
/* 59 */ { 5, s_4_59, 57, 1, 0},
/* 60 */ { 5, s_4_60, 57, 1, 0},
/* 61 */ { 3, s_4_61, 56, 1, 0},
/* 62 */ { 6, s_4_62, 61, 1, 0},
/* 63 */ { 3, s_4_63, 56, 1, 0},
/* 64 */ { 5, s_4_64, 63, 1, 0},
/* 65 */ { 5, s_4_65, 63, 1, 0},
/* 66 */ { 3, s_4_66, 52, 1, 0},
/* 67 */ { 4, s_4_67, 52, 1, 0},
/* 68 */ { 3, s_4_68, 52, 1, 0},
/* 69 */ { 3, s_4_69, 52, 1, 0},
/* 70 */ { 5, s_4_70, 69, 1, 0},
/* 71 */ { 2, s_4_71, -1, 1, 0},
/* 72 */ { 4, s_4_72, 71, 1, 0},
/* 73 */ { 5, s_4_73, 71, 1, 0},
/* 74 */ { 4, s_4_74, 71, 1, 0},
/* 75 */ { 2, s_4_75, -1, 1, 0},
/* 76 */ { 4, s_4_76, 75, 1, 0},
/* 77 */ { 2, s_4_77, -1, 1, 0},
/* 78 */ { 1, s_4_78, -1, 1, 0},
/* 79 */ { 2, s_4_79, 78, 1, 0},
/* 80 */ { 3, s_4_80, 79, 1, 0},
/* 81 */ { 4, s_4_81, 79, 1, 0},
/* 82 */ { 4, s_4_82, 79, 1, 0},
/* 83 */ { 3, s_4_83, 78, 1, 0},
/* 84 */ { 4, s_4_84, 78, 1, 0},
/* 85 */ { 2, s_4_85, 78, 1, 0},
/* 86 */ { 5, s_4_86, 85, 1, 0},
/* 87 */ { 2, s_4_87, 78, 1, 0},
/* 88 */ { 5, s_4_88, 87, 1, 0},
/* 89 */ { 4, s_4_89, 87, 1, 0},
/* 90 */ { 5, s_4_90, 87, 1, 0},
/* 91 */ { 4, s_4_91, 87, 1, 0},
/* 92 */ { 4, s_4_92, 87, 1, 0},
/* 93 */ { 2, s_4_93, 78, 1, 0},
/* 94 */ { 4, s_4_94, 93, 1, 0},
/* 95 */ { 4, s_4_95, 93, 1, 0},
/* 96 */ { 1, s_4_96, -1, 1, 0},
/* 97 */ { 2, s_4_97, 96, 1, 0},
/* 98 */ { 2, s_4_98, -1, 1, 0},
/* 99 */ { 3, s_4_99, 98, 1, 0},
/*100 */ { 4, s_4_100, 99, 1, 0},
/*101 */ { 7, s_4_101, 100, 1, 0},
/*102 */ { 4, s_4_102, 98, 1, 0},
/*103 */ { 4, s_4_103, 98, 1, 0},
/*104 */ { 3, s_4_104, -1, 1, 0},
/*105 */ { 5, s_4_105, 104, 1, 0},
/*106 */ { 3, s_4_106, -1, 1, 0},
/*107 */ { 3, s_4_107, -1, 1, 0},
/*108 */ { 2, s_4_108, -1, 1, 0},
/*109 */ { 3, s_4_109, 108, 1, 0},
/*110 */ { 2, s_4_110, -1, 1, 0},
/*111 */ { 4, s_4_111, 110, 1, 0},
/*112 */ { 2, s_4_112, -1, 1, 0},
/*113 */ { 3, s_4_113, 112, 1, 0},
/*114 */ { 4, s_4_114, 112, 1, 0},
/*115 */ { 2, s_4_115, -1, 1, 0},
/*116 */ { 2, s_4_116, -1, 1, 0},
/*117 */ { 2, s_4_117, -1, 1, 0},
/*118 */ { 1, s_4_118, -1, 1, 0},
/*119 */ { 2, s_4_119, 118, 1, 0},
/*120 */ { 5, s_4_120, 119, 1, 0},
/*121 */ { 3, s_4_121, 118, 1, 0},
/*122 */ { 5, s_4_122, 121, 1, 0},
/*123 */ { 3, s_4_123, 118, 1, 0},
/*124 */ { 2, s_4_124, 118, 1, 0},
/*125 */ { 3, s_4_125, 124, 1, 0},
/*126 */ { 4, s_4_126, 125, 1, 0},
/*127 */ { 7, s_4_127, 126, 1, 0},
/*128 */ { 4, s_4_128, 124, 1, 0},
/*129 */ { 4, s_4_129, 124, 1, 0},
/*130 */ { 3, s_4_130, 118, 1, 0},
/*131 */ { 2, s_4_131, 118, 1, 0},
/*132 */ { 4, s_4_132, 131, 1, 0},
/*133 */ { 5, s_4_133, 131, 1, 0},
/*134 */ { 4, s_4_134, 131, 1, 0},
/*135 */ { 3, s_4_135, 118, 1, 0},
/*136 */ { 2, s_4_136, 118, 1, 0},
/*137 */ { 3, s_4_137, 136, 1, 0},
/*138 */ { 1, s_4_138, -1, 1, 0},
/*139 */ { 2, s_4_139, 138, 1, 0},
/*140 */ { 4, s_4_140, 139, 1, 0},
/*141 */ { 1, s_4_141, -1, 1, 0},
/*142 */ { 3, s_4_142, 141, 1, 0}
};

static const unsigned char g_vowel[] = { 17, 65, 16, 1 };

static const symbol s_0[] = { 'a' };
static const symbol s_1[] = { 'a' };
static const symbol s_2[] = { 'c' };
static const symbol s_3[] = { 'd' };
static const symbol s_4[] = { 'd', 'z' };
static const symbol s_5[] = { 'e' };
static const symbol s_6[] = { 'e' };
static const symbol s_7[] = { 'i' };
static const symbol s_8[] = { 'l' };
static const symbol s_9[] = { 'l' };
static const symbol s_10[] = { 'n' };
static const symbol s_11[] = { 'o' };
static const symbol s_12[] = { 'o' };
static const symbol s_13[] = { 'r' };
static const symbol s_14[] = { 'r' };
static const symbol s_15[] = { 's' };
static const symbol s_16[] = { 't' };
static const symbol s_17[] = { 'u' };
static const symbol s_18[] = { 'u' };
static const symbol s_19[] = { 'y' };
static const symbol s_20[] = { 'z' };
static const symbol s_21[] = { 0xC3, 0xA1 };
static const symbol s_22[] = { 0xC3, 0xA4 };
static const symbol s_23[] = { 0xC4, 0x8D };
static const symbol s_24[] = { 0xC4, 0x8F };
static const symbol s_25[] = { 'd', 0xC5, 0xBE };
static const symbol s_26[] = { 0xC3, 0xA9 };
static const symbol s_27[] = { 0xC4, 0x9B };
static const symbol s_28[] = { 0xC3, 0xAD };
static const symbol s_29[] = { 0xC4, 0xBA };
static const symbol s_30[] = { 0xC4, 0xBE };
static const symbol s_31[] = { 0xC5, 0x88 };
static const symbol s_32[] = { 0xC3, 0xB4 };
static const symbol s_33[] = { 0xC3, 0xB3 };
static const symbol s_34[] = { 0xC5, 0x95 };
static const symbol s_35[] = { 0xC5, 0x99 };
static const symbol s_36[] = { 0xC5, 0xA1 };
static const symbol s_37[] = { 0xC5, 0xA5 };
static const symbol s_38[] = { 0xC3, 0xBA };
static const symbol s_39[] = { 0xC5, 0xAF };
static const symbol s_40[] = { 0xC3, 0xBD };
static const symbol s_41[] = { 0xC5, 0xBE };
static const symbol s_42[] = { 'b', 'y', 't' };
static const symbol s_43[] = { 'm', 'a', 't' };
static const symbol s_44[] = { 'i', 's', 't' };
static const symbol s_45[] = { 'j', 'e', 's', 't' };
static const symbol s_46[] = { 'b', 'a', 'b', 'k' };
static const symbol s_47[] = { 'd', 'l', 'a', 'n' };
static const symbol s_48[] = { 'm', 'a', 'm' };
static const symbol s_49[] = { 'm', 'a', 'j', 'e', 't', 'k' };
static const symbol s_50[] = { 'm', 'e', 's', 't' };
static const symbol s_51[] = { 's', 'e', 'r' };
static const symbol s_52[] = { 'z', 'e', 'n' };

static int r_un_accent(struct SN_env * z) {
    int among_var;
    while(1) { /* repeat, line 70 */
        int c1 = z->c;
        z->bra = z->c; /* [, line 71 */
        among_var = find_among(z, a_0, 22); /* substring, line 71 */
        if (!(among_var)) goto lab0;
        z->ket = z->c; /* ], line 71 */
        switch(among_var) {
            case 0: goto lab0;
            case 1:
                {   int ret = slice_from_s(z, 1, s_0); /* <-, line 72 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_from_s(z, 1, s_1); /* <-, line 73 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {   int ret = slice_from_s(z, 1, s_2); /* <-, line 74 */
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {   int ret = slice_from_s(z, 1, s_3); /* <-, line 75 */
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {   int ret = slice_from_s(z, 2, s_4); /* <-, line 76 */
                    if (ret < 0) return ret;
                }
                break;
            case 6:
                {   int ret = slice_from_s(z, 1, s_5); /* <-, line 77 */
                    if (ret < 0) return ret;
                }
                break;
            case 7:
                {   int ret = slice_from_s(z, 1, s_6); /* <-, line 78 */
                    if (ret < 0) return ret;
                }
                break;
            case 8:
                {   int ret = slice_from_s(z, 1, s_7); /* <-, line 79 */
                    if (ret < 0) return ret;
                }
                break;
            case 9:
                {   int ret = slice_from_s(z, 1, s_8); /* <-, line 80 */
                    if (ret < 0) return ret;
                }
                break;
            case 10:
                {   int ret = slice_from_s(z, 1, s_9); /* <-, line 81 */
                    if (ret < 0) return ret;
                }
                break;
            case 11:
                {   int ret = slice_from_s(z, 1, s_10); /* <-, line 82 */
                    if (ret < 0) return ret;
                }
                break;
            case 12:
                {   int ret = slice_from_s(z, 1, s_11); /* <-, line 83 */
                    if (ret < 0) return ret;
                }
                break;
            case 13:
                {   int ret = slice_from_s(z, 1, s_12); /* <-, line 84 */
                    if (ret < 0) return ret;
                }
                break;
            case 14:
                {   int ret = slice_from_s(z, 1, s_13); /* <-, line 85 */
                    if (ret < 0) return ret;
                }
                break;
            case 15:
                {   int ret = slice_from_s(z, 1, s_14); /* <-, line 86 */
                    if (ret < 0) return ret;
                }
                break;
            case 16:
                {   int ret = slice_from_s(z, 1, s_15); /* <-, line 87 */
                    if (ret < 0) return ret;
                }
                break;
            case 17:
                {   int ret = slice_from_s(z, 1, s_16); /* <-, line 88 */
                    if (ret < 0) return ret;
                }
                break;
            case 18:
                {   int ret = slice_from_s(z, 1, s_17); /* <-, line 89 */
                    if (ret < 0) return ret;
                }
                break;
            case 19:
                {   int ret = slice_from_s(z, 1, s_18); /* <-, line 90 */
                    if (ret < 0) return ret;
                }
                break;
            case 20:
                {   int ret = slice_from_s(z, 1, s_19); /* <-, line 91 */
                    if (ret < 0) return ret;
                }
                break;
            case 21:
                {   int ret = slice_from_s(z, 1, s_20); /* <-, line 92 */
                    if (ret < 0) return ret;
                }
                break;
            case 22:
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret; /* next, line 93 */
                }
                break;
        }
        continue;
    lab0:
        z->c = c1;
        break;
    }
    return 1;
}

static int r_lower_case(struct SN_env * z) {
    int among_var;
    while(1) { /* repeat, line 97 */
        int c1 = z->c;
        z->bra = z->c; /* [, line 98 */
        among_var = find_among(z, a_1, 22); /* substring, line 98 */
        if (!(among_var)) goto lab0;
        z->ket = z->c; /* ], line 98 */
        switch(among_var) {
            case 0: goto lab0;
            case 1:
                {   int ret = slice_from_s(z, 2, s_21); /* <-, line 99 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_from_s(z, 2, s_22); /* <-, line 100 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {   int ret = slice_from_s(z, 2, s_23); /* <-, line 101 */
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {   int ret = slice_from_s(z, 2, s_24); /* <-, line 102 */
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {   int ret = slice_from_s(z, 3, s_25); /* <-, line 103 */
                    if (ret < 0) return ret;
                }
                break;
            case 6:
                {   int ret = slice_from_s(z, 2, s_26); /* <-, line 104 */
                    if (ret < 0) return ret;
                }
                break;
            case 7:
                {   int ret = slice_from_s(z, 2, s_27); /* <-, line 105 */
                    if (ret < 0) return ret;
                }
                break;
            case 8:
                {   int ret = slice_from_s(z, 2, s_28); /* <-, line 106 */
                    if (ret < 0) return ret;
                }
                break;
            case 9:
                {   int ret = slice_from_s(z, 2, s_29); /* <-, line 107 */
                    if (ret < 0) return ret;
                }
                break;
            case 10:
                {   int ret = slice_from_s(z, 2, s_30); /* <-, line 108 */
                    if (ret < 0) return ret;
                }
                break;
            case 11:
                {   int ret = slice_from_s(z, 2, s_31); /* <-, line 109 */
                    if (ret < 0) return ret;
                }
                break;
            case 12:
                {   int ret = slice_from_s(z, 2, s_32); /* <-, line 110 */
                    if (ret < 0) return ret;
                }
                break;
            case 13:
                {   int ret = slice_from_s(z, 2, s_33); /* <-, line 111 */
                    if (ret < 0) return ret;
                }
                break;
            case 14:
                {   int ret = slice_from_s(z, 2, s_34); /* <-, line 112 */
                    if (ret < 0) return ret;
                }
                break;
            case 15:
                {   int ret = slice_from_s(z, 2, s_35); /* <-, line 113 */
                    if (ret < 0) return ret;
                }
                break;
            case 16:
                {   int ret = slice_from_s(z, 2, s_36); /* <-, line 114 */
                    if (ret < 0) return ret;
                }
                break;
            case 17:
                {   int ret = slice_from_s(z, 2, s_37); /* <-, line 115 */
                    if (ret < 0) return ret;
                }
                break;
            case 18:
                {   int ret = slice_from_s(z, 2, s_38); /* <-, line 116 */
                    if (ret < 0) return ret;
                }
                break;
            case 19:
                {   int ret = slice_from_s(z, 2, s_39); /* <-, line 117 */
                    if (ret < 0) return ret;
                }
                break;
            case 20:
                {   int ret = slice_from_s(z, 2, s_40); /* <-, line 118 */
                    if (ret < 0) return ret;
                }
                break;
            case 21:
                {   int ret = slice_from_s(z, 2, s_41); /* <-, line 119 */
                    if (ret < 0) return ret;
                }
                break;
            case 22:
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret; /* next, line 120 */
                }
                break;
        }
        continue;
    lab0:
        z->c = c1;
        break;
    }
    return 1;
}

static int r_exception(struct SN_env * z) {
    int among_var;
    z->bra = z->c; /* [, line 125 */
    if (z->c + 1 >= z->l || z->p[z->c + 1] >> 5 != 3 || !((3978034 >> (z->p[z->c + 1] & 0x1f)) & 1)) return 0;
    among_var = find_among(z, a_2, 85); /* substring, line 125 */
    if (!(among_var)) return 0;
    z->ket = z->c; /* ], line 125 */
    if (z->c < z->l) return 0; /* atlimit, line 125 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_from_s(z, 3, s_42); /* <-, line 132 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_from_s(z, 3, s_43); /* <-, line 137 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {   int ret = slice_from_s(z, 3, s_44); /* <-, line 144 */
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {   int ret = slice_from_s(z, 4, s_45); /* <-, line 149 */
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {   int ret = slice_from_s(z, 4, s_46); /* <-, line 152 */
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {   int ret = slice_from_s(z, 4, s_47); /* <-, line 155 */
                if (ret < 0) return ret;
            }
            break;
        case 7:
            {   int ret = slice_from_s(z, 3, s_48); /* <-, line 158 */
                if (ret < 0) return ret;
            }
            break;
        case 8:
            {   int ret = slice_from_s(z, 6, s_49); /* <-, line 161 */
                if (ret < 0) return ret;
            }
            break;
        case 9:
            {   int ret = slice_from_s(z, 4, s_50); /* <-, line 164 */
                if (ret < 0) return ret;
            }
            break;
        case 10:
            {   int ret = slice_from_s(z, 3, s_51); /* <-, line 167 */
                if (ret < 0) return ret;
            }
            break;
        case 11:
            {   int ret = slice_from_s(z, 3, s_52); /* <-, line 170 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_mark_p1(struct SN_env * z) {
    z->I[0] = z->l;
    {    /* gopast */ /* grouping vowel, line 176 */
        int ret = out_grouping_U(z, g_vowel, 97, 121, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {    /* gopast */ /* non vowel, line 176 */
        int ret = in_grouping_U(z, g_vowel, 97, 121, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[0] = z->c; /* setmark p1, line 176 */
    return 1;
}

static int r_prefixes(struct SN_env * z) {
    int among_var;
    {   int c1 = z->c; /* do, line 180 */
        z->bra = z->c; /* [, line 181 */
        if (z->c + 1 >= z->l || z->p[z->c + 1] >> 5 != 3 || !((100958262 >> (z->p[z->c + 1] & 0x1f)) & 1)) goto lab0;
        among_var = find_among(z, a_3, 17); /* substring, line 181 */
        if (!(among_var)) goto lab0;
        z->ket = z->c; /* ], line 181 */
        switch(among_var) {
            case 0: goto lab0;
            case 1:
                if (!(z->I[0] > 4)) goto lab0;
                {   int ret = slice_del(z); /* delete, line 184 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                if (!(z->I[0] > 2)) goto lab0;
                {   int ret = slice_del(z); /* delete, line 188 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                if (!(z->I[0] > 1)) goto lab0;
                {   int ret = slice_del(z); /* delete, line 192 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        z->c = c1;
    }
    return 1;
}

static int r_R2(struct SN_env * z) {
    if (!(z->I[1] <= z->c)) return 0;
    return 1;
}

static int r_mark_p2(struct SN_env * z) {
    z->I[1] = z->lb;
    return 1;
}

static int r_suffixes(struct SN_env * z) {
    int among_var;
     /* do, line 207 */
    {   int ret = r_mark_p2(z);
        if (ret == 0) goto lab0; /* call mark_p2, line 207 */
        if (ret < 0) return ret;
    }
lab0:
    {   int m1 = z->l - z->c; (void)m1; /* do, line 208 */
        z->ket = z->c; /* [, line 209 */
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((41482026 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab1;
        among_var = find_among_b(z, a_4, 143); /* substring, line 209 */
        if (!(among_var)) goto lab1;
        z->bra = z->c; /* ], line 209 */
        {   int ret = r_R2(z);
            if (ret == 0) goto lab1; /* call R2, line 209 */
            if (ret < 0) return ret;
        }
        switch(among_var) {
            case 0: goto lab1;
            case 1:
                {   int ret = slice_del(z); /* delete, line 233 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab1:
        z->c = z->l - m1;
    }
    return 1;
}

static int r_end_vowel(struct SN_env * z) {
    z->ket = z->c; /* [, line 239 */
    if (in_grouping_b_U(z, g_vowel, 97, 121, 0)) return 0;
    z->bra = z->c; /* ], line 239 */
    {   int ret = slice_del(z); /* delete, line 239 */
        if (ret < 0) return ret;
    }
    return 1;
}

extern int slovak_UTF_8_stem(struct SN_env * z) {
    {   int c1 = z->c; /* do, line 244 */
        {   int ret = r_lower_case(z);
            if (ret == 0) goto lab0; /* call lower_case, line 244 */
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    {   int c2 = z->c; /* do, line 245 */
        {   int ret = r_un_accent(z);
            if (ret == 0) goto lab1; /* call un_accent, line 245 */
            if (ret < 0) return ret;
        }
    lab1:
        z->c = c2;
    }
    {   int c3 = z->c; /* do, line 246 */
        {   int ret = r_mark_p1(z);
            if (ret == 0) goto lab2; /* call mark_p1, line 246 */
            if (ret < 0) return ret;
        }
    lab2:
        z->c = c3;
    }
    {   int c4 = z->c; /* do, line 247 */
        {   int ret = r_prefixes(z);
            if (ret == 0) goto lab3; /* call prefixes, line 247 */
            if (ret < 0) return ret;
        }
    lab3:
        z->c = c4;
    }
    {   int c5 = z->c; /* or, line 248 */
        {   int ret = r_exception(z);
            if (ret == 0) goto lab5; /* call exception, line 248 */
            if (ret < 0) return ret;
        }
        goto lab4;
    lab5:
        z->c = c5;
        z->lb = z->c; z->c = z->l; /* backwards, line 248 */

        {   int m6 = z->l - z->c; (void)m6; /* do, line 249 */
            {   int ret = r_suffixes(z);
                if (ret == 0) goto lab6; /* call suffixes, line 249 */
                if (ret < 0) return ret;
            }
        lab6:
            z->c = z->l - m6;
        }
        {   int m7 = z->l - z->c; (void)m7; /* do, line 250 */
            {   int ret = r_end_vowel(z);
                if (ret == 0) goto lab7; /* call end_vowel, line 250 */
                if (ret < 0) return ret;
            }
        lab7:
            z->c = z->l - m7;
        }
        z->c = z->lb;
    }
lab4:
    return 1;
}

extern struct SN_env * slovak_UTF_8_create_env(void) { return SN_create_env(0, 2, 0); }

extern void slovak_UTF_8_close_env(struct SN_env * z) { SN_close_env(z, 0); }

