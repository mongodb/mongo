
/* This file was generated automatically by the Snowball to ANSI C compiler */

#include "../runtime/header.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int czech_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif
static int r_do_aggressive(struct SN_env * z);
static int r_do_deriv_single(struct SN_env * z);
static int r_do_derivational(struct SN_env * z);
static int r_do_augmentative(struct SN_env * z);
static int r_do_diminutive(struct SN_env * z);
static int r_do_comparative(struct SN_env * z);
static int r_do_case(struct SN_env * z);
static int r_do_possessive(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_palatalise(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_RV(struct SN_env * z);
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * czech_UTF_8_create_env(void);
extern void czech_UTF_8_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_0[2] = { 'c', 'e' };
static const symbol s_0_1[2] = { 'z', 'e' };
static const symbol s_0_2[3] = { 0xC5, 0xBE, 'e' };
static const symbol s_0_3[2] = { 'c', 'i' };
static const symbol s_0_4[4] = { 0xC4, 0x8D, 't', 'i' };
static const symbol s_0_5[4] = { 0xC5, 0xA1, 't', 'i' };
static const symbol s_0_6[2] = { 'z', 'i' };
static const symbol s_0_7[3] = { 0xC4, 0x8D, 'i' };
static const symbol s_0_8[3] = { 0xC5, 0xBE, 'i' };
static const symbol s_0_9[2] = { 0xC4, 0x8D };
static const symbol s_0_10[5] = { 0xC4, 0x8D, 't', 0xC4, 0x9B };
static const symbol s_0_11[5] = { 0xC5, 0xA1, 't', 0xC4, 0x9B };
static const symbol s_0_12[5] = { 0xC4, 0x8D, 't', 0xC3, 0xA9 };
static const symbol s_0_13[5] = { 0xC5, 0xA1, 't', 0xC3, 0xA9 };

static const struct among a_0[14] =
{
/*  0 */ { 2, s_0_0, -1, 1, 0},
/*  1 */ { 2, s_0_1, -1, 2, 0},
/*  2 */ { 3, s_0_2, -1, 2, 0},
/*  3 */ { 2, s_0_3, -1, 1, 0},
/*  4 */ { 4, s_0_4, -1, 3, 0},
/*  5 */ { 4, s_0_5, -1, 4, 0},
/*  6 */ { 2, s_0_6, -1, 2, 0},
/*  7 */ { 3, s_0_7, -1, 1, 0},
/*  8 */ { 3, s_0_8, -1, 2, 0},
/*  9 */ { 2, s_0_9, -1, 1, 0},
/* 10 */ { 5, s_0_10, -1, 3, 0},
/* 11 */ { 5, s_0_11, -1, 4, 0},
/* 12 */ { 5, s_0_12, -1, 3, 0},
/* 13 */ { 5, s_0_13, -1, 4, 0}
};

static const symbol s_1_0[2] = { 'i', 'n' };
static const symbol s_1_1[2] = { 'o', 'v' };
static const symbol s_1_2[3] = { 0xC5, 0xAF, 'v' };

static const struct among a_1[3] =
{
/*  0 */ { 2, s_1_0, -1, 2, 0},
/*  1 */ { 2, s_1_1, -1, 1, 0},
/*  2 */ { 3, s_1_2, -1, 1, 0}
};

static const symbol s_2_0[1] = { 'a' };
static const symbol s_2_1[3] = { 'a', 'm', 'a' };
static const symbol s_2_2[3] = { 'a', 't', 'a' };
static const symbol s_2_3[1] = { 'e' };
static const symbol s_2_4[4] = { 0xC4, 0x9B, 't', 'e' };
static const symbol s_2_5[3] = { 'e', 'c', 'h' };
static const symbol s_2_6[5] = { 'a', 't', 'e', 'c', 'h' };
static const symbol s_2_7[3] = { 'i', 'c', 'h' };
static const symbol s_2_8[4] = { 0xC3, 0xA1, 'c', 'h' };
static const symbol s_2_9[4] = { 0xC3, 0xAD, 'c', 'h' };
static const symbol s_2_10[4] = { 0xC3, 0xBD, 'c', 'h' };
static const symbol s_2_11[1] = { 'i' };
static const symbol s_2_12[2] = { 'm', 'i' };
static const symbol s_2_13[3] = { 'a', 'm', 'i' };
static const symbol s_2_14[3] = { 'e', 'm', 'i' };
static const symbol s_2_15[4] = { 0xC4, 0x9B, 'm', 'i' };
static const symbol s_2_16[4] = { 0xC3, 0xAD, 'm', 'i' };
static const symbol s_2_17[4] = { 0xC3, 0xBD, 'm', 'i' };
static const symbol s_2_18[4] = { 0xC4, 0x9B, 't', 'i' };
static const symbol s_2_19[3] = { 'o', 'v', 'i' };
static const symbol s_2_20[2] = { 'e', 'm' };
static const symbol s_2_21[5] = { 0xC4, 0x9B, 't', 'e', 'm' };
static const symbol s_2_22[3] = { 0xC3, 0xA1, 'm' };
static const symbol s_2_23[3] = { 0xC3, 0xA9, 'm' };
static const symbol s_2_24[3] = { 0xC3, 0xAD, 'm' };
static const symbol s_2_25[5] = { 'a', 't', 0xC5, 0xAF, 'm' };
static const symbol s_2_26[3] = { 0xC3, 0xBD, 'm' };
static const symbol s_2_27[1] = { 'o' };
static const symbol s_2_28[3] = { 'i', 'h', 'o' };
static const symbol s_2_29[4] = { 0xC3, 0xA9, 'h', 'o' };
static const symbol s_2_30[4] = { 0xC3, 0xAD, 'h', 'o' };
static const symbol s_2_31[2] = { 'e', 's' };
static const symbol s_2_32[2] = { 'o', 's' };
static const symbol s_2_33[2] = { 'u', 's' };
static const symbol s_2_34[2] = { 'a', 't' };
static const symbol s_2_35[1] = { 'u' };
static const symbol s_2_36[3] = { 'i', 'm', 'u' };
static const symbol s_2_37[4] = { 0xC3, 0xA9, 'm', 'u' };
static const symbol s_2_38[2] = { 'o', 'u' };
static const symbol s_2_39[1] = { 'y' };
static const symbol s_2_40[3] = { 'a', 't', 'y' };
static const symbol s_2_41[2] = { 0xC4, 0x9B };
static const symbol s_2_42[2] = { 0xC3, 0xA1 };
static const symbol s_2_43[2] = { 0xC3, 0xA9 };
static const symbol s_2_44[4] = { 'o', 'v', 0xC3, 0xA9 };
static const symbol s_2_45[2] = { 0xC3, 0xAD };
static const symbol s_2_46[2] = { 0xC5, 0xAF };
static const symbol s_2_47[2] = { 0xC3, 0xBD };

static const struct among a_2[48] =
{
/*  0 */ { 1, s_2_0, -1, 1, 0},
/*  1 */ { 3, s_2_1, 0, 1, 0},
/*  2 */ { 3, s_2_2, 0, 1, 0},
/*  3 */ { 1, s_2_3, -1, 2, 0},
/*  4 */ { 4, s_2_4, 3, 2, 0},
/*  5 */ { 3, s_2_5, -1, 2, 0},
/*  6 */ { 5, s_2_6, 5, 1, 0},
/*  7 */ { 3, s_2_7, -1, 2, 0},
/*  8 */ { 4, s_2_8, -1, 1, 0},
/*  9 */ { 4, s_2_9, -1, 2, 0},
/* 10 */ { 4, s_2_10, -1, 1, 0},
/* 11 */ { 1, s_2_11, -1, 2, 0},
/* 12 */ { 2, s_2_12, 11, 1, 0},
/* 13 */ { 3, s_2_13, 12, 1, 0},
/* 14 */ { 3, s_2_14, 12, 2, 0},
/* 15 */ { 4, s_2_15, 12, 2, 0},
/* 16 */ { 4, s_2_16, 12, 2, 0},
/* 17 */ { 4, s_2_17, 12, 1, 0},
/* 18 */ { 4, s_2_18, 11, 2, 0},
/* 19 */ { 3, s_2_19, 11, 1, 0},
/* 20 */ { 2, s_2_20, -1, 3, 0},
/* 21 */ { 5, s_2_21, 20, 1, 0},
/* 22 */ { 3, s_2_22, -1, 1, 0},
/* 23 */ { 3, s_2_23, -1, 2, 0},
/* 24 */ { 3, s_2_24, -1, 2, 0},
/* 25 */ { 5, s_2_25, -1, 1, 0},
/* 26 */ { 3, s_2_26, -1, 1, 0},
/* 27 */ { 1, s_2_27, -1, 1, 0},
/* 28 */ { 3, s_2_28, 27, 2, 0},
/* 29 */ { 4, s_2_29, 27, 2, 0},
/* 30 */ { 4, s_2_30, 27, 2, 0},
/* 31 */ { 2, s_2_31, -1, 2, 0},
/* 32 */ { 2, s_2_32, -1, 1, 0},
/* 33 */ { 2, s_2_33, -1, 1, 0},
/* 34 */ { 2, s_2_34, -1, 1, 0},
/* 35 */ { 1, s_2_35, -1, 1, 0},
/* 36 */ { 3, s_2_36, 35, 2, 0},
/* 37 */ { 4, s_2_37, 35, 2, 0},
/* 38 */ { 2, s_2_38, 35, 1, 0},
/* 39 */ { 1, s_2_39, -1, 1, 0},
/* 40 */ { 3, s_2_40, 39, 1, 0},
/* 41 */ { 2, s_2_41, -1, 2, 0},
/* 42 */ { 2, s_2_42, -1, 1, 0},
/* 43 */ { 2, s_2_43, -1, 1, 0},
/* 44 */ { 4, s_2_44, 43, 1, 0},
/* 45 */ { 2, s_2_45, -1, 2, 0},
/* 46 */ { 2, s_2_46, -1, 1, 0},
/* 47 */ { 2, s_2_47, -1, 1, 0}
};

static const symbol s_3_0[2] = { 'o', 'b' };
static const symbol s_3_1[3] = { 'i', 't', 'b' };
static const symbol s_3_2[2] = { 'e', 'c' };
static const symbol s_3_3[4] = { 'i', 'n', 'e', 'c' };
static const symbol s_3_4[6] = { 'o', 'b', 'i', 'n', 'e', 'c' };
static const symbol s_3_5[4] = { 'o', 'v', 'e', 'c' };
static const symbol s_3_6[2] = { 'i', 'c' };
static const symbol s_3_7[4] = { 'e', 'n', 'i', 'c' };
static const symbol s_3_8[3] = { 'o', 'c', 'h' };
static const symbol s_3_9[5] = { 0xC3, 0xA1, 's', 'e', 'k' };
static const symbol s_3_10[2] = { 'n', 'k' };
static const symbol s_3_11[3] = { 'i', 's', 'k' };
static const symbol s_3_12[5] = { 'o', 'v', 'i', 's', 'k' };
static const symbol s_3_13[2] = { 't', 'k' };
static const symbol s_3_14[2] = { 'v', 'k' };
static const symbol s_3_15[3] = { 0xC4, 0x8D, 'k' };
static const symbol s_3_16[4] = { 'i', 0xC5, 0xA1, 'k' };
static const symbol s_3_17[4] = { 'u', 0xC5, 0xA1, 'k' };
static const symbol s_3_18[4] = { 'n', 0xC3, 0xAD, 'k' };
static const symbol s_3_19[6] = { 'o', 'v', 'n', 0xC3, 0xAD, 'k' };
static const symbol s_3_20[5] = { 'o', 'v', 0xC3, 0xAD, 'k' };
static const symbol s_3_21[2] = { 'd', 'l' };
static const symbol s_3_22[4] = { 'i', 't', 'e', 'l' };
static const symbol s_3_23[2] = { 'u', 'l' };
static const symbol s_3_24[2] = { 'a', 'n' };
static const symbol s_3_25[4] = { 0xC4, 0x8D, 'a', 'n' };
static const symbol s_3_26[2] = { 'e', 'n' };
static const symbol s_3_27[2] = { 'i', 'n' };
static const symbol s_3_28[5] = { 0xC5, 0xA1, 't', 'i', 'n' };
static const symbol s_3_29[4] = { 'o', 'v', 'i', 'n' };
static const symbol s_3_30[4] = { 't', 'e', 'l', 'n' };
static const symbol s_3_31[4] = { 0xC3, 0xA1, 'r', 'n' };
static const symbol s_3_32[4] = { 0xC3, 0xAD, 'r', 'n' };
static const symbol s_3_33[3] = { 'o', 'u', 'n' };
static const symbol s_3_34[4] = { 'l', 'o', 'u', 'n' };
static const symbol s_3_35[3] = { 'o', 'v', 'n' };
static const symbol s_3_36[2] = { 'y', 'n' };
static const symbol s_3_37[3] = { 'k', 'y', 'n' };
static const symbol s_3_38[3] = { 0xC4, 0x8D, 'n' };
static const symbol s_3_39[3] = { 0xC4, 0x9B, 'n' };
static const symbol s_3_40[3] = { 0xC3, 0xA1, 'n' };
static const symbol s_3_41[4] = { 'i', 0xC3, 0xA1, 'n' };
static const symbol s_3_42[3] = { 0xC3, 0xAD, 'n' };
static const symbol s_3_43[2] = { 'a', 's' };
static const symbol s_3_44[2] = { 'i', 't' };
static const symbol s_3_45[2] = { 'o', 't' };
static const symbol s_3_46[3] = { 'i', 's', 't' };
static const symbol s_3_47[3] = { 'o', 's', 't' };
static const symbol s_3_48[4] = { 'n', 'o', 's', 't' };
static const symbol s_3_49[3] = { 'o', 'u', 't' };
static const symbol s_3_50[6] = { 'o', 'v', 'i', 0xC5, 0xA1, 't' };
static const symbol s_3_51[2] = { 'i', 'v' };
static const symbol s_3_52[2] = { 'o', 'v' };
static const symbol s_3_53[2] = { 't', 'v' };
static const symbol s_3_54[3] = { 'c', 't', 'v' };
static const symbol s_3_55[3] = { 's', 't', 'v' };
static const symbol s_3_56[5] = { 'o', 'v', 's', 't', 'v' };
static const symbol s_3_57[4] = { 'o', 'v', 't', 'v' };
static const symbol s_3_58[3] = { 'o', 0xC5, 0x88 };
static const symbol s_3_59[3] = { 'a', 0xC4, 0x8D };
static const symbol s_3_60[4] = { 0xC3, 0xA1, 0xC4, 0x8D };
static const symbol s_3_61[4] = { 0xC3, 0xA1, 0xC5, 0x99 };
static const symbol s_3_62[5] = { 'k', 0xC3, 0xA1, 0xC5, 0x99 };
static const symbol s_3_63[7] = { 'i', 'o', 'n', 0xC3, 0xA1, 0xC5, 0x99 };
static const symbol s_3_64[4] = { 0xC3, 0xA9, 0xC5, 0x99 };
static const symbol s_3_65[5] = { 'n', 0xC3, 0xA9, 0xC5, 0x99 };
static const symbol s_3_66[4] = { 0xC3, 0xAD, 0xC5, 0x99 };
static const symbol s_3_67[4] = { 'o', 'u', 0xC5, 0xA1 };

static const struct among a_3[68] =
{
/*  0 */ { 2, s_3_0, -1, 1, 0},
/*  1 */ { 3, s_3_1, -1, 2, 0},
/*  2 */ { 2, s_3_2, -1, 3, 0},
/*  3 */ { 4, s_3_3, 2, 2, 0},
/*  4 */ { 6, s_3_4, 3, 1, 0},
/*  5 */ { 4, s_3_5, 2, 1, 0},
/*  6 */ { 2, s_3_6, -1, 2, 0},
/*  7 */ { 4, s_3_7, 6, 3, 0},
/*  8 */ { 3, s_3_8, -1, 1, 0},
/*  9 */ { 5, s_3_9, -1, 1, 0},
/* 10 */ { 2, s_3_10, -1, 1, 0},
/* 11 */ { 3, s_3_11, -1, 2, 0},
/* 12 */ { 5, s_3_12, 11, 1, 0},
/* 13 */ { 2, s_3_13, -1, 1, 0},
/* 14 */ { 2, s_3_14, -1, 1, 0},
/* 15 */ { 3, s_3_15, -1, 1, 0},
/* 16 */ { 4, s_3_16, -1, 2, 0},
/* 17 */ { 4, s_3_17, -1, 1, 0},
/* 18 */ { 4, s_3_18, -1, 1, 0},
/* 19 */ { 6, s_3_19, 18, 1, 0},
/* 20 */ { 5, s_3_20, -1, 1, 0},
/* 21 */ { 2, s_3_21, -1, 1, 0},
/* 22 */ { 4, s_3_22, -1, 2, 0},
/* 23 */ { 2, s_3_23, -1, 1, 0},
/* 24 */ { 2, s_3_24, -1, 1, 0},
/* 25 */ { 4, s_3_25, 24, 1, 0},
/* 26 */ { 2, s_3_26, -1, 3, 0},
/* 27 */ { 2, s_3_27, -1, 2, 0},
/* 28 */ { 5, s_3_28, 27, 1, 0},
/* 29 */ { 4, s_3_29, 27, 1, 0},
/* 30 */ { 4, s_3_30, -1, 1, 0},
/* 31 */ { 4, s_3_31, -1, 1, 0},
/* 32 */ { 4, s_3_32, -1, 6, 0},
/* 33 */ { 3, s_3_33, -1, 1, 0},
/* 34 */ { 4, s_3_34, 33, 1, 0},
/* 35 */ { 3, s_3_35, -1, 1, 0},
/* 36 */ { 2, s_3_36, -1, 1, 0},
/* 37 */ { 3, s_3_37, 36, 1, 0},
/* 38 */ { 3, s_3_38, -1, 1, 0},
/* 39 */ { 3, s_3_39, -1, 5, 0},
/* 40 */ { 3, s_3_40, -1, 1, 0},
/* 41 */ { 4, s_3_41, 40, 2, 0},
/* 42 */ { 3, s_3_42, -1, 6, 0},
/* 43 */ { 2, s_3_43, -1, 1, 0},
/* 44 */ { 2, s_3_44, -1, 2, 0},
/* 45 */ { 2, s_3_45, -1, 1, 0},
/* 46 */ { 3, s_3_46, -1, 2, 0},
/* 47 */ { 3, s_3_47, -1, 1, 0},
/* 48 */ { 4, s_3_48, 47, 1, 0},
/* 49 */ { 3, s_3_49, -1, 1, 0},
/* 50 */ { 6, s_3_50, -1, 1, 0},
/* 51 */ { 2, s_3_51, -1, 2, 0},
/* 52 */ { 2, s_3_52, -1, 1, 0},
/* 53 */ { 2, s_3_53, -1, 1, 0},
/* 54 */ { 3, s_3_54, 53, 1, 0},
/* 55 */ { 3, s_3_55, 53, 1, 0},
/* 56 */ { 5, s_3_56, 55, 1, 0},
/* 57 */ { 4, s_3_57, 53, 1, 0},
/* 58 */ { 3, s_3_58, -1, 1, 0},
/* 59 */ { 3, s_3_59, -1, 1, 0},
/* 60 */ { 4, s_3_60, -1, 1, 0},
/* 61 */ { 4, s_3_61, -1, 1, 0},
/* 62 */ { 5, s_3_62, 61, 1, 0},
/* 63 */ { 7, s_3_63, 61, 2, 0},
/* 64 */ { 4, s_3_64, -1, 4, 0},
/* 65 */ { 5, s_3_65, 64, 1, 0},
/* 66 */ { 4, s_3_66, -1, 6, 0},
/* 67 */ { 4, s_3_67, -1, 1, 0}
};

static const symbol s_4_0[1] = { 'c' };
static const symbol s_4_1[1] = { 'k' };
static const symbol s_4_2[1] = { 'l' };
static const symbol s_4_3[1] = { 'n' };
static const symbol s_4_4[1] = { 't' };
static const symbol s_4_5[2] = { 0xC4, 0x8D };

static const struct among a_4[6] =
{
/*  0 */ { 1, s_4_0, -1, 1, 0},
/*  1 */ { 1, s_4_1, -1, 1, 0},
/*  2 */ { 1, s_4_2, -1, 1, 0},
/*  3 */ { 1, s_4_3, -1, 1, 0},
/*  4 */ { 1, s_4_4, -1, 1, 0},
/*  5 */ { 2, s_4_5, -1, 1, 0}
};

static const symbol s_5_0[3] = { 'i', 's', 'k' };
static const symbol s_5_1[3] = { 0xC3, 0xA1, 'k' };
static const symbol s_5_2[3] = { 'i', 'z', 'n' };
static const symbol s_5_3[4] = { 'a', 'j', 'z', 'n' };

static const struct among a_5[4] =
{
/*  0 */ { 3, s_5_0, -1, 2, 0},
/*  1 */ { 3, s_5_1, -1, 1, 0},
/*  2 */ { 3, s_5_2, -1, 2, 0},
/*  3 */ { 4, s_5_3, -1, 1, 0}
};

static const symbol s_6_0[1] = { 'k' };
static const symbol s_6_1[2] = { 'a', 'k' };
static const symbol s_6_2[2] = { 'e', 'k' };
static const symbol s_6_3[4] = { 'a', 'n', 'e', 'k' };
static const symbol s_6_4[4] = { 'e', 'n', 'e', 'k' };
static const symbol s_6_5[4] = { 'i', 'n', 'e', 'k' };
static const symbol s_6_6[4] = { 'o', 'n', 'e', 'k' };
static const symbol s_6_7[4] = { 'u', 'n', 'e', 'k' };
static const symbol s_6_8[5] = { 0xC3, 0xA1, 'n', 'e', 'k' };
static const symbol s_6_9[5] = { 'a', 0xC4, 0x8D, 'e', 'k' };
static const symbol s_6_10[5] = { 'e', 0xC4, 0x8D, 'e', 'k' };
static const symbol s_6_11[5] = { 'i', 0xC4, 0x8D, 'e', 'k' };
static const symbol s_6_12[5] = { 'o', 0xC4, 0x8D, 'e', 'k' };
static const symbol s_6_13[5] = { 'u', 0xC4, 0x8D, 'e', 'k' };
static const symbol s_6_14[6] = { 0xC3, 0xA1, 0xC4, 0x8D, 'e', 'k' };
static const symbol s_6_15[6] = { 0xC3, 0xA9, 0xC4, 0x8D, 'e', 'k' };
static const symbol s_6_16[6] = { 0xC3, 0xAD, 0xC4, 0x8D, 'e', 'k' };
static const symbol s_6_17[6] = { 'o', 'u', 0xC5, 0xA1, 'e', 'k' };
static const symbol s_6_18[2] = { 'i', 'k' };
static const symbol s_6_19[3] = { 'a', 'n', 'k' };
static const symbol s_6_20[3] = { 'e', 'n', 'k' };
static const symbol s_6_21[3] = { 'i', 'n', 'k' };
static const symbol s_6_22[3] = { 'o', 'n', 'k' };
static const symbol s_6_23[3] = { 'u', 'n', 'k' };
static const symbol s_6_24[4] = { 0xC3, 0xA1, 'n', 'k' };
static const symbol s_6_25[4] = { 0xC3, 0xA9, 'n', 'k' };
static const symbol s_6_26[4] = { 0xC3, 0xAD, 'n', 'k' };
static const symbol s_6_27[2] = { 'o', 'k' };
static const symbol s_6_28[4] = { 0xC3, 0xA1, 't', 'k' };
static const symbol s_6_29[2] = { 'u', 'k' };
static const symbol s_6_30[4] = { 'a', 0xC4, 0x8D, 'k' };
static const symbol s_6_31[4] = { 'e', 0xC4, 0x8D, 'k' };
static const symbol s_6_32[4] = { 'i', 0xC4, 0x8D, 'k' };
static const symbol s_6_33[4] = { 'o', 0xC4, 0x8D, 'k' };
static const symbol s_6_34[4] = { 'u', 0xC4, 0x8D, 'k' };
static const symbol s_6_35[5] = { 0xC3, 0xA1, 0xC4, 0x8D, 'k' };
static const symbol s_6_36[5] = { 0xC3, 0xA9, 0xC4, 0x8D, 'k' };
static const symbol s_6_37[5] = { 0xC3, 0xAD, 0xC4, 0x8D, 'k' };
static const symbol s_6_38[3] = { 0xC3, 0xA1, 'k' };
static const symbol s_6_39[4] = { 'u', 0xC5, 0xA1, 'k' };
static const symbol s_6_40[3] = { 0xC3, 0xA9, 'k' };
static const symbol s_6_41[3] = { 0xC3, 0xAD, 'k' };

static const struct among a_6[42] =
{
/*  0 */ { 1, s_6_0, -1, 1, 0},
/*  1 */ { 2, s_6_1, 0, 7, 0},
/*  2 */ { 2, s_6_2, 0, 2, 0},
/*  3 */ { 4, s_6_3, 2, 1, 0},
/*  4 */ { 4, s_6_4, 2, 2, 0},
/*  5 */ { 4, s_6_5, 2, 4, 0},
/*  6 */ { 4, s_6_6, 2, 1, 0},
/*  7 */ { 4, s_6_7, 2, 1, 0},
/*  8 */ { 5, s_6_8, 2, 1, 0},
/*  9 */ { 5, s_6_9, 2, 1, 0},
/* 10 */ { 5, s_6_10, 2, 2, 0},
/* 11 */ { 5, s_6_11, 2, 4, 0},
/* 12 */ { 5, s_6_12, 2, 1, 0},
/* 13 */ { 5, s_6_13, 2, 1, 0},
/* 14 */ { 6, s_6_14, 2, 1, 0},
/* 15 */ { 6, s_6_15, 2, 3, 0},
/* 16 */ { 6, s_6_16, 2, 5, 0},
/* 17 */ { 6, s_6_17, 2, 1, 0},
/* 18 */ { 2, s_6_18, 0, 4, 0},
/* 19 */ { 3, s_6_19, 0, 1, 0},
/* 20 */ { 3, s_6_20, 0, 1, 0},
/* 21 */ { 3, s_6_21, 0, 1, 0},
/* 22 */ { 3, s_6_22, 0, 1, 0},
/* 23 */ { 3, s_6_23, 0, 1, 0},
/* 24 */ { 4, s_6_24, 0, 1, 0},
/* 25 */ { 4, s_6_25, 0, 1, 0},
/* 26 */ { 4, s_6_26, 0, 1, 0},
/* 27 */ { 2, s_6_27, 0, 8, 0},
/* 28 */ { 4, s_6_28, 0, 1, 0},
/* 29 */ { 2, s_6_29, 0, 9, 0},
/* 30 */ { 4, s_6_30, 0, 1, 0},
/* 31 */ { 4, s_6_31, 0, 1, 0},
/* 32 */ { 4, s_6_32, 0, 1, 0},
/* 33 */ { 4, s_6_33, 0, 1, 0},
/* 34 */ { 4, s_6_34, 0, 1, 0},
/* 35 */ { 5, s_6_35, 0, 1, 0},
/* 36 */ { 5, s_6_36, 0, 1, 0},
/* 37 */ { 5, s_6_37, 0, 1, 0},
/* 38 */ { 3, s_6_38, 0, 6, 0},
/* 39 */ { 4, s_6_39, 0, 1, 0},
/* 40 */ { 3, s_6_40, 0, 3, 0},
/* 41 */ { 3, s_6_41, 0, 5, 0}
};

static const symbol s_7_0[4] = { 'e', 'j', 0xC5, 0xA1 };
static const symbol s_7_1[5] = { 0xC4, 0x9B, 'j', 0xC5, 0xA1 };

static const struct among a_7[2] =
{
/*  0 */ { 4, s_7_0, -1, 2, 0},
/*  1 */ { 5, s_7_1, -1, 1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 17, 4, 18, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 };

static const symbol s_0[] = { 'k' };
static const symbol s_1[] = { 'h' };
static const symbol s_2[] = { 'c', 'k' };
static const symbol s_3[] = { 's', 'k' };
static const symbol s_4[] = { 'e' };
static const symbol s_5[] = { 'i' };
static const symbol s_6[] = { 'e' };
static const symbol s_7[] = { 0xC3, 0xA9 };
static const symbol s_8[] = { 0xC4, 0x9B };
static const symbol s_9[] = { 0xC3, 0xAD };
static const symbol s_10[] = { 'i' };
static const symbol s_11[] = { 'e' };
static const symbol s_12[] = { 0xC3, 0xA9 };
static const symbol s_13[] = { 'i' };
static const symbol s_14[] = { 0xC3, 0xAD };
static const symbol s_15[] = { 0xC3, 0xA1 };
static const symbol s_16[] = { 'a' };
static const symbol s_17[] = { 'o' };
static const symbol s_18[] = { 'u' };
static const symbol s_19[] = { 0xC4, 0x9B };
static const symbol s_20[] = { 'e' };

static int r_mark_regions(struct SN_env * z) {
    z->I[0] = z->l;
    z->I[1] = z->l;
    {   int c1 = z->c; /* do, line 46 */
        {    /* gopast */ /* non v, line 47 */
            int ret = in_grouping_U(z, g_v, 97, 367, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        z->I[0] = z->c; /* setmark pV, line 47 */
        {    /* gopast */ /* non v, line 48 */
            int ret = in_grouping_U(z, g_v, 97, 367, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {    /* gopast */ /* grouping v, line 48 */
            int ret = out_grouping_U(z, g_v, 97, 367, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        z->I[1] = z->c; /* setmark p1, line 48 */
    lab0:
        z->c = c1;
    }
    return 1;
}

static int r_RV(struct SN_env * z) {
    if (!(z->I[0] <= z->c)) return 0;
    return 1;
}

static int r_R1(struct SN_env * z) {
    if (!(z->I[1] <= z->c)) return 0;
    return 1;
}

static int r_palatalise(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 58 */
    among_var = find_among_b(z, a_0, 14); /* substring, line 58 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 58 */
    {   int ret = r_RV(z);
        if (ret == 0) return 0; /* call RV, line 58 */
        if (ret < 0) return ret;
    }
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_from_s(z, 1, s_0); /* <-, line 60 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_from_s(z, 1, s_1); /* <-, line 62 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {   int ret = slice_from_s(z, 2, s_2); /* <-, line 64 */
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {   int ret = slice_from_s(z, 2, s_3); /* <-, line 66 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_do_possessive(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 71 */
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 110 && z->p[z->c - 1] != 118)) return 0;
    among_var = find_among_b(z, a_1, 3); /* substring, line 71 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 71 */
    {   int ret = r_RV(z);
        if (ret == 0) return 0; /* call RV, line 71 */
        if (ret < 0) return ret;
    }
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 73 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 76 */
                if (ret < 0) return ret;
            }
            {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 77 */
                {   int ret = r_palatalise(z);
                    if (ret == 0) { z->c = z->l - m_keep; goto lab0; } /* call palatalise, line 77 */
                    if (ret < 0) return ret;
                }
            lab0:
                ;
            }
            break;
    }
    return 1;
}

static int r_do_case(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 83 */
    among_var = find_among_b(z, a_2, 48); /* substring, line 83 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 83 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 90 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 97 */
                if (ret < 0) return ret;
            }
            {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 98 */
                {   int ret = r_palatalise(z);
                    if (ret == 0) { z->c = z->l - m_keep; goto lab0; } /* call palatalise, line 98 */
                    if (ret < 0) return ret;
                }
            lab0:
                ;
            }
            break;
        case 3:
            {   int ret = slice_from_s(z, 1, s_4); /* <-, line 102 */
                if (ret < 0) return ret;
            }
            {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 103 */
                {   int ret = r_palatalise(z);
                    if (ret == 0) { z->c = z->l - m_keep; goto lab1; } /* call palatalise, line 103 */
                    if (ret < 0) return ret;
                }
            lab1:
                ;
            }
            break;
    }
    return 1;
}

static int r_do_derivational(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 109 */
    among_var = find_among_b(z, a_3, 68); /* substring, line 109 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 109 */
    {   int ret = r_R1(z);
        if (ret == 0) return 0; /* call R1, line 109 */
        if (ret < 0) return ret;
    }
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 118 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_from_s(z, 1, s_5); /* <-, line 124 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 125 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {   int ret = slice_from_s(z, 1, s_6); /* <-, line 129 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 130 */
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {   int ret = slice_from_s(z, 2, s_7); /* <-, line 134 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 135 */
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {   int ret = slice_from_s(z, 2, s_8); /* <-, line 139 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 140 */
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {   int ret = slice_from_s(z, 2, s_9); /* <-, line 145 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 146 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_do_deriv_single(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 151 */
    among_var = find_among_b(z, a_4, 6); /* substring, line 151 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 151 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 153 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_do_augmentative(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 159 */
    if (z->c - 2 <= z->lb || (z->p[z->c - 1] != 107 && z->p[z->c - 1] != 110)) return 0;
    among_var = find_among_b(z, a_5, 4); /* substring, line 159 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 159 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 161 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_from_s(z, 1, s_10); /* <-, line 164 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 165 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_do_diminutive(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 171 */
    if (z->c <= z->lb || z->p[z->c - 1] != 107) return 0;
    among_var = find_among_b(z, a_6, 42); /* substring, line 171 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 171 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 178 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_from_s(z, 1, s_11); /* <-, line 181 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 182 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {   int ret = slice_from_s(z, 2, s_12); /* <-, line 186 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 187 */
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {   int ret = slice_from_s(z, 1, s_13); /* <-, line 191 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 192 */
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {   int ret = slice_from_s(z, 2, s_14); /* <-, line 196 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 197 */
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {   int ret = slice_from_s(z, 2, s_15); /* <-, line 200 */
                if (ret < 0) return ret;
            }
            break;
        case 7:
            {   int ret = slice_from_s(z, 1, s_16); /* <-, line 202 */
                if (ret < 0) return ret;
            }
            break;
        case 8:
            {   int ret = slice_from_s(z, 1, s_17); /* <-, line 204 */
                if (ret < 0) return ret;
            }
            break;
        case 9:
            {   int ret = slice_from_s(z, 1, s_18); /* <-, line 206 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_do_comparative(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 211 */
    if (z->c - 3 <= z->lb || z->p[z->c - 1] != 161) return 0;
    among_var = find_among_b(z, a_7, 2); /* substring, line 211 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 211 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_from_s(z, 2, s_19); /* <-, line 214 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 215 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_from_s(z, 1, s_20); /* <-, line 219 */
                if (ret < 0) return ret;
            }
            {   int ret = r_palatalise(z);
                if (ret == 0) return 0; /* call palatalise, line 220 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_do_aggressive(struct SN_env * z) {
    {   int m1 = z->l - z->c; (void)m1; /* do, line 226 */
        {   int ret = r_do_comparative(z);
            if (ret == 0) goto lab0; /* call do_comparative, line 226 */
            if (ret < 0) return ret;
        }
    lab0:
        z->c = z->l - m1;
    }
    {   int m2 = z->l - z->c; (void)m2; /* do, line 227 */
        {   int ret = r_do_diminutive(z);
            if (ret == 0) goto lab1; /* call do_diminutive, line 227 */
            if (ret < 0) return ret;
        }
    lab1:
        z->c = z->l - m2;
    }
    {   int m3 = z->l - z->c; (void)m3; /* do, line 228 */
        {   int ret = r_do_augmentative(z);
            if (ret == 0) goto lab2; /* call do_augmentative, line 228 */
            if (ret < 0) return ret;
        }
    lab2:
        z->c = z->l - m3;
    }
    {   int m4 = z->l - z->c; (void)m4; /* or, line 229 */
        {   int ret = r_do_derivational(z);
            if (ret == 0) goto lab4; /* call do_derivational, line 229 */
            if (ret < 0) return ret;
        }
        goto lab3;
    lab4:
        z->c = z->l - m4;
        {   int ret = r_do_deriv_single(z);
            if (ret == 0) return 0; /* call do_deriv_single, line 229 */
            if (ret < 0) return ret;
        }
    }
lab3:
    return 1;
}

extern int czech_UTF_8_stem(struct SN_env * z) {
    {   int c1 = z->c; /* do, line 234 */
        {   int ret = r_mark_regions(z);
            if (ret == 0) goto lab0; /* call mark_regions, line 234 */
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    z->lb = z->c; z->c = z->l; /* backwards, line 235 */

    {   int ret = r_do_case(z);
        if (ret == 0) return 0; /* call do_case, line 236 */
        if (ret < 0) return ret;
    }
    {   int ret = r_do_possessive(z);
        if (ret == 0) return 0; /* call do_possessive, line 237 */
        if (ret < 0) return ret;
    }
    {   int ret = r_do_aggressive(z);
        if (ret == 0) return 0; /* call do_aggressive, line 240 */
        if (ret < 0) return ret;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * czech_UTF_8_create_env(void) { return SN_create_env(0, 2, 0); }

extern void czech_UTF_8_close_env(struct SN_env * z) { SN_close_env(z, 0); }

