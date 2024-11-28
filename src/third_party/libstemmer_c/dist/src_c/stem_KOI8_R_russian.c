
/* This file was generated automatically by the Snowball to ANSI C compiler */

#include "../runtime/header.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int russian_KOI8_R_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif
static int r_tidy_up(struct SN_env * z);
static int r_derivational(struct SN_env * z);
static int r_noun(struct SN_env * z);
static int r_verb(struct SN_env * z);
static int r_reflexive(struct SN_env * z);
static int r_adjectival(struct SN_env * z);
static int r_adjective(struct SN_env * z);
static int r_perfective_gerund(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * russian_KOI8_R_create_env(void);
extern void russian_KOI8_R_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_0[3] = { 0xD7, 0xDB, 0xC9 };
static const symbol s_0_1[4] = { 0xC9, 0xD7, 0xDB, 0xC9 };
static const symbol s_0_2[4] = { 0xD9, 0xD7, 0xDB, 0xC9 };
static const symbol s_0_3[1] = { 0xD7 };
static const symbol s_0_4[2] = { 0xC9, 0xD7 };
static const symbol s_0_5[2] = { 0xD9, 0xD7 };
static const symbol s_0_6[5] = { 0xD7, 0xDB, 0xC9, 0xD3, 0xD8 };
static const symbol s_0_7[6] = { 0xC9, 0xD7, 0xDB, 0xC9, 0xD3, 0xD8 };
static const symbol s_0_8[6] = { 0xD9, 0xD7, 0xDB, 0xC9, 0xD3, 0xD8 };

static const struct among a_0[9] =
{
/*  0 */ { 3, s_0_0, -1, 1, 0},
/*  1 */ { 4, s_0_1, 0, 2, 0},
/*  2 */ { 4, s_0_2, 0, 2, 0},
/*  3 */ { 1, s_0_3, -1, 1, 0},
/*  4 */ { 2, s_0_4, 3, 2, 0},
/*  5 */ { 2, s_0_5, 3, 2, 0},
/*  6 */ { 5, s_0_6, -1, 1, 0},
/*  7 */ { 6, s_0_7, 6, 2, 0},
/*  8 */ { 6, s_0_8, 6, 2, 0}
};

static const symbol s_1_0[2] = { 0xC0, 0xC0 };
static const symbol s_1_1[2] = { 0xC5, 0xC0 };
static const symbol s_1_2[2] = { 0xCF, 0xC0 };
static const symbol s_1_3[2] = { 0xD5, 0xC0 };
static const symbol s_1_4[2] = { 0xC5, 0xC5 };
static const symbol s_1_5[2] = { 0xC9, 0xC5 };
static const symbol s_1_6[2] = { 0xCF, 0xC5 };
static const symbol s_1_7[2] = { 0xD9, 0xC5 };
static const symbol s_1_8[2] = { 0xC9, 0xC8 };
static const symbol s_1_9[2] = { 0xD9, 0xC8 };
static const symbol s_1_10[3] = { 0xC9, 0xCD, 0xC9 };
static const symbol s_1_11[3] = { 0xD9, 0xCD, 0xC9 };
static const symbol s_1_12[2] = { 0xC5, 0xCA };
static const symbol s_1_13[2] = { 0xC9, 0xCA };
static const symbol s_1_14[2] = { 0xCF, 0xCA };
static const symbol s_1_15[2] = { 0xD9, 0xCA };
static const symbol s_1_16[2] = { 0xC5, 0xCD };
static const symbol s_1_17[2] = { 0xC9, 0xCD };
static const symbol s_1_18[2] = { 0xCF, 0xCD };
static const symbol s_1_19[2] = { 0xD9, 0xCD };
static const symbol s_1_20[3] = { 0xC5, 0xC7, 0xCF };
static const symbol s_1_21[3] = { 0xCF, 0xC7, 0xCF };
static const symbol s_1_22[2] = { 0xC1, 0xD1 };
static const symbol s_1_23[2] = { 0xD1, 0xD1 };
static const symbol s_1_24[3] = { 0xC5, 0xCD, 0xD5 };
static const symbol s_1_25[3] = { 0xCF, 0xCD, 0xD5 };

static const struct among a_1[26] =
{
/*  0 */ { 2, s_1_0, -1, 1, 0},
/*  1 */ { 2, s_1_1, -1, 1, 0},
/*  2 */ { 2, s_1_2, -1, 1, 0},
/*  3 */ { 2, s_1_3, -1, 1, 0},
/*  4 */ { 2, s_1_4, -1, 1, 0},
/*  5 */ { 2, s_1_5, -1, 1, 0},
/*  6 */ { 2, s_1_6, -1, 1, 0},
/*  7 */ { 2, s_1_7, -1, 1, 0},
/*  8 */ { 2, s_1_8, -1, 1, 0},
/*  9 */ { 2, s_1_9, -1, 1, 0},
/* 10 */ { 3, s_1_10, -1, 1, 0},
/* 11 */ { 3, s_1_11, -1, 1, 0},
/* 12 */ { 2, s_1_12, -1, 1, 0},
/* 13 */ { 2, s_1_13, -1, 1, 0},
/* 14 */ { 2, s_1_14, -1, 1, 0},
/* 15 */ { 2, s_1_15, -1, 1, 0},
/* 16 */ { 2, s_1_16, -1, 1, 0},
/* 17 */ { 2, s_1_17, -1, 1, 0},
/* 18 */ { 2, s_1_18, -1, 1, 0},
/* 19 */ { 2, s_1_19, -1, 1, 0},
/* 20 */ { 3, s_1_20, -1, 1, 0},
/* 21 */ { 3, s_1_21, -1, 1, 0},
/* 22 */ { 2, s_1_22, -1, 1, 0},
/* 23 */ { 2, s_1_23, -1, 1, 0},
/* 24 */ { 3, s_1_24, -1, 1, 0},
/* 25 */ { 3, s_1_25, -1, 1, 0}
};

static const symbol s_2_0[2] = { 0xC5, 0xCD };
static const symbol s_2_1[2] = { 0xCE, 0xCE };
static const symbol s_2_2[2] = { 0xD7, 0xDB };
static const symbol s_2_3[3] = { 0xC9, 0xD7, 0xDB };
static const symbol s_2_4[3] = { 0xD9, 0xD7, 0xDB };
static const symbol s_2_5[1] = { 0xDD };
static const symbol s_2_6[2] = { 0xC0, 0xDD };
static const symbol s_2_7[3] = { 0xD5, 0xC0, 0xDD };

static const struct among a_2[8] =
{
/*  0 */ { 2, s_2_0, -1, 1, 0},
/*  1 */ { 2, s_2_1, -1, 1, 0},
/*  2 */ { 2, s_2_2, -1, 1, 0},
/*  3 */ { 3, s_2_3, 2, 2, 0},
/*  4 */ { 3, s_2_4, 2, 2, 0},
/*  5 */ { 1, s_2_5, -1, 1, 0},
/*  6 */ { 2, s_2_6, 5, 1, 0},
/*  7 */ { 3, s_2_7, 6, 2, 0}
};

static const symbol s_3_0[2] = { 0xD3, 0xD1 };
static const symbol s_3_1[2] = { 0xD3, 0xD8 };

static const struct among a_3[2] =
{
/*  0 */ { 2, s_3_0, -1, 1, 0},
/*  1 */ { 2, s_3_1, -1, 1, 0}
};

static const symbol s_4_0[1] = { 0xC0 };
static const symbol s_4_1[2] = { 0xD5, 0xC0 };
static const symbol s_4_2[2] = { 0xCC, 0xC1 };
static const symbol s_4_3[3] = { 0xC9, 0xCC, 0xC1 };
static const symbol s_4_4[3] = { 0xD9, 0xCC, 0xC1 };
static const symbol s_4_5[2] = { 0xCE, 0xC1 };
static const symbol s_4_6[3] = { 0xC5, 0xCE, 0xC1 };
static const symbol s_4_7[3] = { 0xC5, 0xD4, 0xC5 };
static const symbol s_4_8[3] = { 0xC9, 0xD4, 0xC5 };
static const symbol s_4_9[3] = { 0xCA, 0xD4, 0xC5 };
static const symbol s_4_10[4] = { 0xC5, 0xCA, 0xD4, 0xC5 };
static const symbol s_4_11[4] = { 0xD5, 0xCA, 0xD4, 0xC5 };
static const symbol s_4_12[2] = { 0xCC, 0xC9 };
static const symbol s_4_13[3] = { 0xC9, 0xCC, 0xC9 };
static const symbol s_4_14[3] = { 0xD9, 0xCC, 0xC9 };
static const symbol s_4_15[1] = { 0xCA };
static const symbol s_4_16[2] = { 0xC5, 0xCA };
static const symbol s_4_17[2] = { 0xD5, 0xCA };
static const symbol s_4_18[1] = { 0xCC };
static const symbol s_4_19[2] = { 0xC9, 0xCC };
static const symbol s_4_20[2] = { 0xD9, 0xCC };
static const symbol s_4_21[2] = { 0xC5, 0xCD };
static const symbol s_4_22[2] = { 0xC9, 0xCD };
static const symbol s_4_23[2] = { 0xD9, 0xCD };
static const symbol s_4_24[1] = { 0xCE };
static const symbol s_4_25[2] = { 0xC5, 0xCE };
static const symbol s_4_26[2] = { 0xCC, 0xCF };
static const symbol s_4_27[3] = { 0xC9, 0xCC, 0xCF };
static const symbol s_4_28[3] = { 0xD9, 0xCC, 0xCF };
static const symbol s_4_29[2] = { 0xCE, 0xCF };
static const symbol s_4_30[3] = { 0xC5, 0xCE, 0xCF };
static const symbol s_4_31[3] = { 0xCE, 0xCE, 0xCF };
static const symbol s_4_32[2] = { 0xC0, 0xD4 };
static const symbol s_4_33[3] = { 0xD5, 0xC0, 0xD4 };
static const symbol s_4_34[2] = { 0xC5, 0xD4 };
static const symbol s_4_35[3] = { 0xD5, 0xC5, 0xD4 };
static const symbol s_4_36[2] = { 0xC9, 0xD4 };
static const symbol s_4_37[2] = { 0xD1, 0xD4 };
static const symbol s_4_38[2] = { 0xD9, 0xD4 };
static const symbol s_4_39[2] = { 0xD4, 0xD8 };
static const symbol s_4_40[3] = { 0xC9, 0xD4, 0xD8 };
static const symbol s_4_41[3] = { 0xD9, 0xD4, 0xD8 };
static const symbol s_4_42[3] = { 0xC5, 0xDB, 0xD8 };
static const symbol s_4_43[3] = { 0xC9, 0xDB, 0xD8 };
static const symbol s_4_44[2] = { 0xCE, 0xD9 };
static const symbol s_4_45[3] = { 0xC5, 0xCE, 0xD9 };

static const struct among a_4[46] =
{
/*  0 */ { 1, s_4_0, -1, 2, 0},
/*  1 */ { 2, s_4_1, 0, 2, 0},
/*  2 */ { 2, s_4_2, -1, 1, 0},
/*  3 */ { 3, s_4_3, 2, 2, 0},
/*  4 */ { 3, s_4_4, 2, 2, 0},
/*  5 */ { 2, s_4_5, -1, 1, 0},
/*  6 */ { 3, s_4_6, 5, 2, 0},
/*  7 */ { 3, s_4_7, -1, 1, 0},
/*  8 */ { 3, s_4_8, -1, 2, 0},
/*  9 */ { 3, s_4_9, -1, 1, 0},
/* 10 */ { 4, s_4_10, 9, 2, 0},
/* 11 */ { 4, s_4_11, 9, 2, 0},
/* 12 */ { 2, s_4_12, -1, 1, 0},
/* 13 */ { 3, s_4_13, 12, 2, 0},
/* 14 */ { 3, s_4_14, 12, 2, 0},
/* 15 */ { 1, s_4_15, -1, 1, 0},
/* 16 */ { 2, s_4_16, 15, 2, 0},
/* 17 */ { 2, s_4_17, 15, 2, 0},
/* 18 */ { 1, s_4_18, -1, 1, 0},
/* 19 */ { 2, s_4_19, 18, 2, 0},
/* 20 */ { 2, s_4_20, 18, 2, 0},
/* 21 */ { 2, s_4_21, -1, 1, 0},
/* 22 */ { 2, s_4_22, -1, 2, 0},
/* 23 */ { 2, s_4_23, -1, 2, 0},
/* 24 */ { 1, s_4_24, -1, 1, 0},
/* 25 */ { 2, s_4_25, 24, 2, 0},
/* 26 */ { 2, s_4_26, -1, 1, 0},
/* 27 */ { 3, s_4_27, 26, 2, 0},
/* 28 */ { 3, s_4_28, 26, 2, 0},
/* 29 */ { 2, s_4_29, -1, 1, 0},
/* 30 */ { 3, s_4_30, 29, 2, 0},
/* 31 */ { 3, s_4_31, 29, 1, 0},
/* 32 */ { 2, s_4_32, -1, 1, 0},
/* 33 */ { 3, s_4_33, 32, 2, 0},
/* 34 */ { 2, s_4_34, -1, 1, 0},
/* 35 */ { 3, s_4_35, 34, 2, 0},
/* 36 */ { 2, s_4_36, -1, 2, 0},
/* 37 */ { 2, s_4_37, -1, 2, 0},
/* 38 */ { 2, s_4_38, -1, 2, 0},
/* 39 */ { 2, s_4_39, -1, 1, 0},
/* 40 */ { 3, s_4_40, 39, 2, 0},
/* 41 */ { 3, s_4_41, 39, 2, 0},
/* 42 */ { 3, s_4_42, -1, 1, 0},
/* 43 */ { 3, s_4_43, -1, 2, 0},
/* 44 */ { 2, s_4_44, -1, 1, 0},
/* 45 */ { 3, s_4_45, 44, 2, 0}
};

static const symbol s_5_0[1] = { 0xC0 };
static const symbol s_5_1[2] = { 0xC9, 0xC0 };
static const symbol s_5_2[2] = { 0xD8, 0xC0 };
static const symbol s_5_3[1] = { 0xC1 };
static const symbol s_5_4[1] = { 0xC5 };
static const symbol s_5_5[2] = { 0xC9, 0xC5 };
static const symbol s_5_6[2] = { 0xD8, 0xC5 };
static const symbol s_5_7[2] = { 0xC1, 0xC8 };
static const symbol s_5_8[2] = { 0xD1, 0xC8 };
static const symbol s_5_9[3] = { 0xC9, 0xD1, 0xC8 };
static const symbol s_5_10[1] = { 0xC9 };
static const symbol s_5_11[2] = { 0xC5, 0xC9 };
static const symbol s_5_12[2] = { 0xC9, 0xC9 };
static const symbol s_5_13[3] = { 0xC1, 0xCD, 0xC9 };
static const symbol s_5_14[3] = { 0xD1, 0xCD, 0xC9 };
static const symbol s_5_15[4] = { 0xC9, 0xD1, 0xCD, 0xC9 };
static const symbol s_5_16[1] = { 0xCA };
static const symbol s_5_17[2] = { 0xC5, 0xCA };
static const symbol s_5_18[3] = { 0xC9, 0xC5, 0xCA };
static const symbol s_5_19[2] = { 0xC9, 0xCA };
static const symbol s_5_20[2] = { 0xCF, 0xCA };
static const symbol s_5_21[2] = { 0xC1, 0xCD };
static const symbol s_5_22[2] = { 0xC5, 0xCD };
static const symbol s_5_23[3] = { 0xC9, 0xC5, 0xCD };
static const symbol s_5_24[2] = { 0xCF, 0xCD };
static const symbol s_5_25[2] = { 0xD1, 0xCD };
static const symbol s_5_26[3] = { 0xC9, 0xD1, 0xCD };
static const symbol s_5_27[1] = { 0xCF };
static const symbol s_5_28[1] = { 0xD1 };
static const symbol s_5_29[2] = { 0xC9, 0xD1 };
static const symbol s_5_30[2] = { 0xD8, 0xD1 };
static const symbol s_5_31[1] = { 0xD5 };
static const symbol s_5_32[2] = { 0xC5, 0xD7 };
static const symbol s_5_33[2] = { 0xCF, 0xD7 };
static const symbol s_5_34[1] = { 0xD8 };
static const symbol s_5_35[1] = { 0xD9 };

static const struct among a_5[36] =
{
/*  0 */ { 1, s_5_0, -1, 1, 0},
/*  1 */ { 2, s_5_1, 0, 1, 0},
/*  2 */ { 2, s_5_2, 0, 1, 0},
/*  3 */ { 1, s_5_3, -1, 1, 0},
/*  4 */ { 1, s_5_4, -1, 1, 0},
/*  5 */ { 2, s_5_5, 4, 1, 0},
/*  6 */ { 2, s_5_6, 4, 1, 0},
/*  7 */ { 2, s_5_7, -1, 1, 0},
/*  8 */ { 2, s_5_8, -1, 1, 0},
/*  9 */ { 3, s_5_9, 8, 1, 0},
/* 10 */ { 1, s_5_10, -1, 1, 0},
/* 11 */ { 2, s_5_11, 10, 1, 0},
/* 12 */ { 2, s_5_12, 10, 1, 0},
/* 13 */ { 3, s_5_13, 10, 1, 0},
/* 14 */ { 3, s_5_14, 10, 1, 0},
/* 15 */ { 4, s_5_15, 14, 1, 0},
/* 16 */ { 1, s_5_16, -1, 1, 0},
/* 17 */ { 2, s_5_17, 16, 1, 0},
/* 18 */ { 3, s_5_18, 17, 1, 0},
/* 19 */ { 2, s_5_19, 16, 1, 0},
/* 20 */ { 2, s_5_20, 16, 1, 0},
/* 21 */ { 2, s_5_21, -1, 1, 0},
/* 22 */ { 2, s_5_22, -1, 1, 0},
/* 23 */ { 3, s_5_23, 22, 1, 0},
/* 24 */ { 2, s_5_24, -1, 1, 0},
/* 25 */ { 2, s_5_25, -1, 1, 0},
/* 26 */ { 3, s_5_26, 25, 1, 0},
/* 27 */ { 1, s_5_27, -1, 1, 0},
/* 28 */ { 1, s_5_28, -1, 1, 0},
/* 29 */ { 2, s_5_29, 28, 1, 0},
/* 30 */ { 2, s_5_30, 28, 1, 0},
/* 31 */ { 1, s_5_31, -1, 1, 0},
/* 32 */ { 2, s_5_32, -1, 1, 0},
/* 33 */ { 2, s_5_33, -1, 1, 0},
/* 34 */ { 1, s_5_34, -1, 1, 0},
/* 35 */ { 1, s_5_35, -1, 1, 0}
};

static const symbol s_6_0[3] = { 0xCF, 0xD3, 0xD4 };
static const symbol s_6_1[4] = { 0xCF, 0xD3, 0xD4, 0xD8 };

static const struct among a_6[2] =
{
/*  0 */ { 3, s_6_0, -1, 1, 0},
/*  1 */ { 4, s_6_1, -1, 1, 0}
};

static const symbol s_7_0[4] = { 0xC5, 0xCA, 0xDB, 0xC5 };
static const symbol s_7_1[1] = { 0xCE };
static const symbol s_7_2[1] = { 0xD8 };
static const symbol s_7_3[3] = { 0xC5, 0xCA, 0xDB };

static const struct among a_7[4] =
{
/*  0 */ { 4, s_7_0, -1, 1, 0},
/*  1 */ { 1, s_7_1, -1, 2, 0},
/*  2 */ { 1, s_7_2, -1, 3, 0},
/*  3 */ { 3, s_7_3, -1, 1, 0}
};

static const unsigned char g_v[] = { 35, 130, 34, 18 };

static const symbol s_0[] = { 0xC1 };
static const symbol s_1[] = { 0xD1 };
static const symbol s_2[] = { 0xC1 };
static const symbol s_3[] = { 0xD1 };
static const symbol s_4[] = { 0xC1 };
static const symbol s_5[] = { 0xD1 };
static const symbol s_6[] = { 0xCE };
static const symbol s_7[] = { 0xCE };
static const symbol s_8[] = { 0xCE };
static const symbol s_9[] = { 0xC9 };

static int r_mark_regions(struct SN_env * z) {
    z->I[0] = z->l;
    z->I[1] = z->l;
    {   int c1 = z->c; /* do, line 63 */
        {    /* gopast */ /* grouping v, line 64 */
            int ret = out_grouping(z, g_v, 192, 220, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        z->I[0] = z->c; /* setmark pV, line 64 */
        {    /* gopast */ /* non v, line 64 */
            int ret = in_grouping(z, g_v, 192, 220, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {    /* gopast */ /* grouping v, line 65 */
            int ret = out_grouping(z, g_v, 192, 220, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {    /* gopast */ /* non v, line 65 */
            int ret = in_grouping(z, g_v, 192, 220, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        z->I[1] = z->c; /* setmark p2, line 65 */
    lab0:
        z->c = c1;
    }
    return 1;
}

static int r_R2(struct SN_env * z) {
    if (!(z->I[1] <= z->c)) return 0;
    return 1;
}

static int r_perfective_gerund(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 74 */
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((25166336 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_0, 9); /* substring, line 74 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 74 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int m1 = z->l - z->c; (void)m1; /* or, line 78 */
                if (!(eq_s_b(z, 1, s_0))) goto lab1;
                goto lab0;
            lab1:
                z->c = z->l - m1;
                if (!(eq_s_b(z, 1, s_1))) return 0;
            }
        lab0:
            {   int ret = slice_del(z); /* delete, line 78 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 85 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_adjective(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 90 */
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((2271009 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_1, 26); /* substring, line 90 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 90 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 99 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_adjectival(struct SN_env * z) {
    int among_var;
    {   int ret = r_adjective(z);
        if (ret == 0) return 0; /* call adjective, line 104 */
        if (ret < 0) return ret;
    }
    {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 111 */
        z->ket = z->c; /* [, line 112 */
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((671113216 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->c = z->l - m_keep; goto lab0; }
        among_var = find_among_b(z, a_2, 8); /* substring, line 112 */
        if (!(among_var)) { z->c = z->l - m_keep; goto lab0; }
        z->bra = z->c; /* ], line 112 */
        switch(among_var) {
            case 0: { z->c = z->l - m_keep; goto lab0; }
            case 1:
                {   int m1 = z->l - z->c; (void)m1; /* or, line 117 */
                    if (!(eq_s_b(z, 1, s_2))) goto lab2;
                    goto lab1;
                lab2:
                    z->c = z->l - m1;
                    if (!(eq_s_b(z, 1, s_3))) { z->c = z->l - m_keep; goto lab0; }
                }
            lab1:
                {   int ret = slice_del(z); /* delete, line 117 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_del(z); /* delete, line 124 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        ;
    }
    return 1;
}

static int r_reflexive(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 131 */
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 209 && z->p[z->c - 1] != 216)) return 0;
    among_var = find_among_b(z, a_3, 2); /* substring, line 131 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 131 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 134 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_verb(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 139 */
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((51443235 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_4, 46); /* substring, line 139 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 139 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int m1 = z->l - z->c; (void)m1; /* or, line 145 */
                if (!(eq_s_b(z, 1, s_4))) goto lab1;
                goto lab0;
            lab1:
                z->c = z->l - m1;
                if (!(eq_s_b(z, 1, s_5))) return 0;
            }
        lab0:
            {   int ret = slice_del(z); /* delete, line 145 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 153 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_noun(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 162 */
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((60991267 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_5, 36); /* substring, line 162 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 162 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 169 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_derivational(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 178 */
    if (z->c - 2 <= z->lb || (z->p[z->c - 1] != 212 && z->p[z->c - 1] != 216)) return 0;
    among_var = find_among_b(z, a_6, 2); /* substring, line 178 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 178 */
    {   int ret = r_R2(z);
        if (ret == 0) return 0; /* call R2, line 178 */
        if (ret < 0) return ret;
    }
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 181 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_tidy_up(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 186 */
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((151011360 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_7, 4); /* substring, line 186 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 186 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 190 */
                if (ret < 0) return ret;
            }
            z->ket = z->c; /* [, line 191 */
            if (!(eq_s_b(z, 1, s_6))) return 0;
            z->bra = z->c; /* ], line 191 */
            if (!(eq_s_b(z, 1, s_7))) return 0;
            {   int ret = slice_del(z); /* delete, line 191 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(eq_s_b(z, 1, s_8))) return 0;
            {   int ret = slice_del(z); /* delete, line 194 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {   int ret = slice_del(z); /* delete, line 196 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int russian_KOI8_R_stem(struct SN_env * z) {
    {   int c1 = z->c; /* do, line 203 */
        {   int ret = r_mark_regions(z);
            if (ret == 0) goto lab0; /* call mark_regions, line 203 */
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    z->lb = z->c; z->c = z->l; /* backwards, line 204 */

    {   int mlimit; /* setlimit, line 204 */
        int m2 = z->l - z->c; (void)m2;
        if (z->c < z->I[0]) return 0;
        z->c = z->I[0]; /* tomark, line 204 */
        mlimit = z->lb; z->lb = z->c;
        z->c = z->l - m2;
        {   int m3 = z->l - z->c; (void)m3; /* do, line 205 */
            {   int m4 = z->l - z->c; (void)m4; /* or, line 206 */
                {   int ret = r_perfective_gerund(z);
                    if (ret == 0) goto lab3; /* call perfective_gerund, line 206 */
                    if (ret < 0) return ret;
                }
                goto lab2;
            lab3:
                z->c = z->l - m4;
                {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 207 */
                    {   int ret = r_reflexive(z);
                        if (ret == 0) { z->c = z->l - m_keep; goto lab4; } /* call reflexive, line 207 */
                        if (ret < 0) return ret;
                    }
                lab4:
                    ;
                }
                {   int m5 = z->l - z->c; (void)m5; /* or, line 208 */
                    {   int ret = r_adjectival(z);
                        if (ret == 0) goto lab6; /* call adjectival, line 208 */
                        if (ret < 0) return ret;
                    }
                    goto lab5;
                lab6:
                    z->c = z->l - m5;
                    {   int ret = r_verb(z);
                        if (ret == 0) goto lab7; /* call verb, line 208 */
                        if (ret < 0) return ret;
                    }
                    goto lab5;
                lab7:
                    z->c = z->l - m5;
                    {   int ret = r_noun(z);
                        if (ret == 0) goto lab1; /* call noun, line 208 */
                        if (ret < 0) return ret;
                    }
                }
            lab5:
                ;
            }
        lab2:
        lab1:
            z->c = z->l - m3;
        }
        {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 211 */
            z->ket = z->c; /* [, line 211 */
            if (!(eq_s_b(z, 1, s_9))) { z->c = z->l - m_keep; goto lab8; }
            z->bra = z->c; /* ], line 211 */
            {   int ret = slice_del(z); /* delete, line 211 */
                if (ret < 0) return ret;
            }
        lab8:
            ;
        }
        {   int m6 = z->l - z->c; (void)m6; /* do, line 214 */
            {   int ret = r_derivational(z);
                if (ret == 0) goto lab9; /* call derivational, line 214 */
                if (ret < 0) return ret;
            }
        lab9:
            z->c = z->l - m6;
        }
        {   int m7 = z->l - z->c; (void)m7; /* do, line 215 */
            {   int ret = r_tidy_up(z);
                if (ret == 0) goto lab10; /* call tidy_up, line 215 */
                if (ret < 0) return ret;
            }
        lab10:
            z->c = z->l - m7;
        }
        z->lb = mlimit;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * russian_KOI8_R_create_env(void) { return SN_create_env(0, 2, 0); }

extern void russian_KOI8_R_close_env(struct SN_env * z) { SN_close_env(z, 0); }

