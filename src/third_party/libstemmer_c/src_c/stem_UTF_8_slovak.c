
/* This file was generated automatically by the Snowball to ANSI C compiler */

#include "../runtime/header.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int slovak_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif
static int r_do_case(struct SN_env * z);
static int r_do_possessive(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_RV(struct SN_env * z);
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * slovak_UTF_8_create_env(void);
extern void slovak_UTF_8_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_0[2] = { 'i', 'n' };
static const symbol s_0_1[2] = { 'o', 'v' };

static const struct among a_0[2] =
{
/*  0 */ { 2, s_0_0, -1, 2, 0},
/*  1 */ { 2, s_0_1, -1, 1, 0}
};

static const symbol s_1_0[1] = { 'a' };
static const symbol s_1_1[2] = { 'i', 'a' };
static const symbol s_1_2[5] = { 'e', 'j', 's', 'i', 'a' };
static const symbol s_1_3[4] = { 'o', 'v', 'i', 'a' };
static const symbol s_1_4[6] = { 'e', 'j', 0xC5, 0xA1, 'i', 'a' };
static const symbol s_1_5[3] = { 'i', 'l', 'a' };
static const symbol s_1_6[3] = { 'a', 't', 'a' };
static const symbol s_1_7[3] = { 'i', 'a', 'c' };
static const symbol s_1_8[5] = { 'e', 'n', 'i', 'e', 'c' };
static const symbol s_1_9[2] = { 'u', 'c' };
static const symbol s_1_10[3] = { 0xC3, 0xBA, 'c' };
static const symbol s_1_11[1] = { 'e' };
static const symbol s_1_12[4] = { 'e', 'n', 'c', 'e' };
static const symbol s_1_13[2] = { 'i', 'e' };
static const symbol s_1_14[5] = { 'e', 'j', 's', 'i', 'e' };
static const symbol s_1_15[6] = { 'e', 'j', 0xC5, 0xA1, 'i', 'e' };
static const symbol s_1_16[2] = { 'm', 'e' };
static const symbol s_1_17[3] = { 'e', 'm', 'e' };
static const symbol s_1_18[4] = { 'i', 'e', 'm', 'e' };
static const symbol s_1_19[3] = { 'i', 'm', 'e' };
static const symbol s_1_20[4] = { 0xC3, 0xAD, 'm', 'e' };
static const symbol s_1_21[2] = { 't', 'e' };
static const symbol s_1_22[4] = { 'i', 'e', 't', 'e' };
static const symbol s_1_23[3] = { 'i', 't', 'e' };
static const symbol s_1_24[4] = { 0xC3, 0xAD, 't', 'e' };
static const symbol s_1_25[3] = { 'o', 'v', 'e' };
static const symbol s_1_26[3] = { 'a', 'c', 'h' };
static const symbol s_1_27[4] = { 'i', 'a', 'c', 'h' };
static const symbol s_1_28[3] = { 'i', 'c', 'h' };
static const symbol s_1_29[6] = { 'e', 'j', 's', 'i', 'c', 'h' };
static const symbol s_1_30[7] = { 'e', 'j', 0xC5, 0xA1, 'i', 'c', 'h' };
static const symbol s_1_31[3] = { 'o', 'c', 'h' };
static const symbol s_1_32[6] = { 'e', 'n', 'c', 'o', 'c', 'h' };
static const symbol s_1_33[3] = { 'y', 'c', 'h' };
static const symbol s_1_34[4] = { 0xC3, 0xA1, 'c', 'h' };
static const symbol s_1_35[6] = { 'a', 't', 0xC3, 0xA1, 'c', 'h' };
static const symbol s_1_36[4] = { 0xC3, 0xAD, 'c', 'h' };
static const symbol s_1_37[4] = { 0xC3, 0xBD, 'c', 'h' };
static const symbol s_1_38[1] = { 'i' };
static const symbol s_1_39[3] = { 'i', 'l', 'i' };
static const symbol s_1_40[2] = { 'm', 'i' };
static const symbol s_1_41[3] = { 'a', 'm', 'i' };
static const symbol s_1_42[6] = { 'e', 'n', 'c', 'a', 'm', 'i' };
static const symbol s_1_43[4] = { 'i', 'a', 'm', 'i' };
static const symbol s_1_44[5] = { 'a', 't', 'a', 'm', 'i' };
static const symbol s_1_45[3] = { 'e', 'm', 'i' };
static const symbol s_1_46[3] = { 'i', 'm', 'i' };
static const symbol s_1_47[6] = { 'e', 'j', 's', 'i', 'm', 'i' };
static const symbol s_1_48[7] = { 'e', 'j', 0xC5, 0xA1, 'i', 'm', 'i' };
static const symbol s_1_49[3] = { 'y', 'm', 'i' };
static const symbol s_1_50[4] = { 0xC3, 0xAD, 'm', 'i' };
static const symbol s_1_51[4] = { 0xC3, 0xBD, 'm', 'i' };
static const symbol s_1_52[4] = { 'e', 'j', 's', 'i' };
static const symbol s_1_53[3] = { 'o', 'v', 'i' };
static const symbol s_1_54[5] = { 'e', 'j', 0xC5, 0xA1, 'i' };
static const symbol s_1_55[5] = { 'e', 'j', 's', 'e', 'j' };
static const symbol s_1_56[6] = { 'e', 'j', 0xC5, 0xA1, 'e', 'j' };
static const symbol s_1_57[2] = { 'i', 'l' };
static const symbol s_1_58[3] = { 0xC3, 0xAD, 'l' };
static const symbol s_1_59[2] = { 'a', 'm' };
static const symbol s_1_60[3] = { 'i', 'a', 'm' };
static const symbol s_1_61[4] = { 'a', 't', 'a', 'm' };
static const symbol s_1_62[2] = { 'i', 'm' };
static const symbol s_1_63[5] = { 'e', 'j', 's', 'i', 'm' };
static const symbol s_1_64[6] = { 'e', 'j', 0xC5, 0xA1, 'i', 'm' };
static const symbol s_1_65[5] = { 'e', 'n', 'c', 'o', 'm' };
static const symbol s_1_66[5] = { 'e', 'j', 's', 'o', 'm' };
static const symbol s_1_67[4] = { 'a', 't', 'o', 'm' };
static const symbol s_1_68[4] = { 'e', 't', 'o', 'm' };
static const symbol s_1_69[6] = { 'e', 'j', 0xC5, 0xA1, 'o', 'm' };
static const symbol s_1_70[5] = { 'a', 0xC5, 0xA5, 'o', 'm' };
static const symbol s_1_71[5] = { 'e', 0xC5, 0xA5, 'o', 'm' };
static const symbol s_1_72[2] = { 'y', 'm' };
static const symbol s_1_73[3] = { 0xC3, 0xA1, 'm' };
static const symbol s_1_74[5] = { 'a', 't', 0xC3, 0xA1, 'm' };
static const symbol s_1_75[3] = { 0xC3, 0xAD, 'm' };
static const symbol s_1_76[3] = { 0xC3, 0xBD, 'm' };
static const symbol s_1_77[1] = { 'o' };
static const symbol s_1_78[2] = { 'h', 'o' };
static const symbol s_1_79[3] = { 'e', 'h', 'o' };
static const symbol s_1_80[4] = { 'i', 'e', 'h', 'o' };
static const symbol s_1_81[7] = { 'e', 'j', 's', 'i', 'e', 'h', 'o' };
static const symbol s_1_82[8] = { 'e', 'j', 0xC5, 0xA1, 'i', 'e', 'h', 'o' };
static const symbol s_1_83[4] = { 0xC3, 0xA9, 'h', 'o' };
static const symbol s_1_84[3] = { 'i', 'l', 'o' };
static const symbol s_1_85[2] = { 'a', 's' };
static const symbol s_1_86[2] = { 'e', 's' };
static const symbol s_1_87[3] = { 'i', 'e', 's' };
static const symbol s_1_88[2] = { 'i', 's' };
static const symbol s_1_89[2] = { 'u', 's' };
static const symbol s_1_90[2] = { 'a', 't' };
static const symbol s_1_91[3] = { 'i', 'e', 't' };
static const symbol s_1_92[2] = { 'i', 't' };
static const symbol s_1_93[2] = { 'u', 't' };
static const symbol s_1_94[1] = { 'u' };
static const symbol s_1_95[2] = { 'i', 'u' };
static const symbol s_1_96[5] = { 'e', 'j', 's', 'i', 'u' };
static const symbol s_1_97[6] = { 'e', 'j', 0xC5, 0xA1, 'i', 'u' };
static const symbol s_1_98[3] = { 'a', 'j', 'u' };
static const symbol s_1_99[3] = { 'e', 'j', 'u' };
static const symbol s_1_100[3] = { 'u', 'j', 'u' };
static const symbol s_1_101[2] = { 'm', 'u' };
static const symbol s_1_102[4] = { 'i', 'e', 'm', 'u' };
static const symbol s_1_103[7] = { 'e', 'j', 's', 'i', 'e', 'm', 'u' };
static const symbol s_1_104[8] = { 'e', 'j', 0xC5, 0xA1, 'i', 'e', 'm', 'u' };
static const symbol s_1_105[2] = { 'o', 'u' };
static const symbol s_1_106[5] = { 'e', 'j', 's', 'o', 'u' };
static const symbol s_1_107[6] = { 'e', 'j', 0xC5, 0xA1, 'o', 'u' };
static const symbol s_1_108[1] = { 'y' };
static const symbol s_1_109[3] = { 'o', 'v', 'y' };
static const symbol s_1_110[2] = { 0xC3, 0xA1 };
static const symbol s_1_111[4] = { 'a', 't', 0xC3, 0xA1 };
static const symbol s_1_112[3] = { 'a', 0xC5, 0xA1 };
static const symbol s_1_113[3] = { 'e', 0xC5, 0xA1 };
static const symbol s_1_114[4] = { 'i', 'e', 0xC5, 0xA1 };
static const symbol s_1_115[3] = { 'i', 0xC5, 0xA1 };
static const symbol s_1_116[4] = { 0xC3, 0xAD, 0xC5, 0xA1 };
static const symbol s_1_117[3] = { 'a', 0xC5, 0xA5 };
static const symbol s_1_118[4] = { 'i', 'e', 0xC5, 0xA5 };
static const symbol s_1_119[3] = { 'i', 0xC5, 0xA5 };
static const symbol s_1_120[4] = { 0xC3, 0xBA, 0xC5, 0xA5 };
static const symbol s_1_121[2] = { 0xC3, 0xA9 };
static const symbol s_1_122[4] = { 'o', 'v', 0xC3, 0xA9 };
static const symbol s_1_123[2] = { 0xC3, 0xAD };
static const symbol s_1_124[4] = { 'o', 'v', 0xC3, 0xAD };
static const symbol s_1_125[2] = { 0xC3, 0xB3 };
static const symbol s_1_126[2] = { 0xC3, 0xBA };
static const symbol s_1_127[4] = { 'a', 'j', 0xC3, 0xBA };
static const symbol s_1_128[4] = { 'e', 'j', 0xC3, 0xBA };
static const symbol s_1_129[4] = { 'u', 'j', 0xC3, 0xBA };
static const symbol s_1_130[2] = { 0xC3, 0xBD };
static const symbol s_1_131[4] = { 'o', 'v', 0xC3, 0xBD };

static const struct among a_1[132] =
{
/*  0 */ { 1, s_1_0, -1, 1, 0},
/*  1 */ { 2, s_1_1, 0, 1, 0},
/*  2 */ { 5, s_1_2, 1, 1, 0},
/*  3 */ { 4, s_1_3, 1, 1, 0},
/*  4 */ { 6, s_1_4, 1, 1, 0},
/*  5 */ { 3, s_1_5, 0, 1, 0},
/*  6 */ { 3, s_1_6, 0, 1, 0},
/*  7 */ { 3, s_1_7, -1, 1, 0},
/*  8 */ { 5, s_1_8, -1, 1, 0},
/*  9 */ { 2, s_1_9, -1, 1, 0},
/* 10 */ { 3, s_1_10, -1, 1, 0},
/* 11 */ { 1, s_1_11, -1, 1, 0},
/* 12 */ { 4, s_1_12, 11, 1, 0},
/* 13 */ { 2, s_1_13, 11, 1, 0},
/* 14 */ { 5, s_1_14, 13, 1, 0},
/* 15 */ { 6, s_1_15, 13, 1, 0},
/* 16 */ { 2, s_1_16, 11, 1, 0},
/* 17 */ { 3, s_1_17, 16, 1, 0},
/* 18 */ { 4, s_1_18, 17, 1, 0},
/* 19 */ { 3, s_1_19, 16, 1, 0},
/* 20 */ { 4, s_1_20, 16, 1, 0},
/* 21 */ { 2, s_1_21, 11, 1, 0},
/* 22 */ { 4, s_1_22, 21, 1, 0},
/* 23 */ { 3, s_1_23, 21, 1, 0},
/* 24 */ { 4, s_1_24, 21, 1, 0},
/* 25 */ { 3, s_1_25, 11, 1, 0},
/* 26 */ { 3, s_1_26, -1, 1, 0},
/* 27 */ { 4, s_1_27, 26, 1, 0},
/* 28 */ { 3, s_1_28, -1, 1, 0},
/* 29 */ { 6, s_1_29, 28, 1, 0},
/* 30 */ { 7, s_1_30, 28, 1, 0},
/* 31 */ { 3, s_1_31, -1, 1, 0},
/* 32 */ { 6, s_1_32, 31, 1, 0},
/* 33 */ { 3, s_1_33, -1, 1, 0},
/* 34 */ { 4, s_1_34, -1, 1, 0},
/* 35 */ { 6, s_1_35, 34, 1, 0},
/* 36 */ { 4, s_1_36, -1, 1, 0},
/* 37 */ { 4, s_1_37, -1, 1, 0},
/* 38 */ { 1, s_1_38, -1, 1, 0},
/* 39 */ { 3, s_1_39, 38, 1, 0},
/* 40 */ { 2, s_1_40, 38, 1, 0},
/* 41 */ { 3, s_1_41, 40, 1, 0},
/* 42 */ { 6, s_1_42, 41, 1, 0},
/* 43 */ { 4, s_1_43, 41, 1, 0},
/* 44 */ { 5, s_1_44, 41, 1, 0},
/* 45 */ { 3, s_1_45, 40, 1, 0},
/* 46 */ { 3, s_1_46, 40, 1, 0},
/* 47 */ { 6, s_1_47, 46, 1, 0},
/* 48 */ { 7, s_1_48, 46, 1, 0},
/* 49 */ { 3, s_1_49, 40, 1, 0},
/* 50 */ { 4, s_1_50, 40, 1, 0},
/* 51 */ { 4, s_1_51, 40, 1, 0},
/* 52 */ { 4, s_1_52, 38, 1, 0},
/* 53 */ { 3, s_1_53, 38, 1, 0},
/* 54 */ { 5, s_1_54, 38, 1, 0},
/* 55 */ { 5, s_1_55, -1, 1, 0},
/* 56 */ { 6, s_1_56, -1, 1, 0},
/* 57 */ { 2, s_1_57, -1, 1, 0},
/* 58 */ { 3, s_1_58, -1, 1, 0},
/* 59 */ { 2, s_1_59, -1, 1, 0},
/* 60 */ { 3, s_1_60, 59, 1, 0},
/* 61 */ { 4, s_1_61, 59, 1, 0},
/* 62 */ { 2, s_1_62, -1, 1, 0},
/* 63 */ { 5, s_1_63, 62, 1, 0},
/* 64 */ { 6, s_1_64, 62, 1, 0},
/* 65 */ { 5, s_1_65, -1, 1, 0},
/* 66 */ { 5, s_1_66, -1, 1, 0},
/* 67 */ { 4, s_1_67, -1, 1, 0},
/* 68 */ { 4, s_1_68, -1, 1, 0},
/* 69 */ { 6, s_1_69, -1, 1, 0},
/* 70 */ { 5, s_1_70, -1, 1, 0},
/* 71 */ { 5, s_1_71, -1, 1, 0},
/* 72 */ { 2, s_1_72, -1, 1, 0},
/* 73 */ { 3, s_1_73, -1, 1, 0},
/* 74 */ { 5, s_1_74, 73, 1, 0},
/* 75 */ { 3, s_1_75, -1, 1, 0},
/* 76 */ { 3, s_1_76, -1, 1, 0},
/* 77 */ { 1, s_1_77, -1, 1, 0},
/* 78 */ { 2, s_1_78, 77, 1, 0},
/* 79 */ { 3, s_1_79, 78, 1, 0},
/* 80 */ { 4, s_1_80, 79, 1, 0},
/* 81 */ { 7, s_1_81, 80, 1, 0},
/* 82 */ { 8, s_1_82, 80, 1, 0},
/* 83 */ { 4, s_1_83, 78, 1, 0},
/* 84 */ { 3, s_1_84, 77, 1, 0},
/* 85 */ { 2, s_1_85, -1, 1, 0},
/* 86 */ { 2, s_1_86, -1, 1, 0},
/* 87 */ { 3, s_1_87, 86, 1, 0},
/* 88 */ { 2, s_1_88, -1, 1, 0},
/* 89 */ { 2, s_1_89, -1, 1, 0},
/* 90 */ { 2, s_1_90, -1, 1, 0},
/* 91 */ { 3, s_1_91, -1, 1, 0},
/* 92 */ { 2, s_1_92, -1, 1, 0},
/* 93 */ { 2, s_1_93, -1, 1, 0},
/* 94 */ { 1, s_1_94, -1, 1, 0},
/* 95 */ { 2, s_1_95, 94, 1, 0},
/* 96 */ { 5, s_1_96, 95, 1, 0},
/* 97 */ { 6, s_1_97, 95, 1, 0},
/* 98 */ { 3, s_1_98, 94, 1, 0},
/* 99 */ { 3, s_1_99, 94, 1, 0},
/*100 */ { 3, s_1_100, 94, 1, 0},
/*101 */ { 2, s_1_101, 94, 1, 0},
/*102 */ { 4, s_1_102, 101, 1, 0},
/*103 */ { 7, s_1_103, 102, 1, 0},
/*104 */ { 8, s_1_104, 102, 1, 0},
/*105 */ { 2, s_1_105, 94, 1, 0},
/*106 */ { 5, s_1_106, 105, 1, 0},
/*107 */ { 6, s_1_107, 105, 1, 0},
/*108 */ { 1, s_1_108, -1, 1, 0},
/*109 */ { 3, s_1_109, 108, 1, 0},
/*110 */ { 2, s_1_110, -1, 1, 0},
/*111 */ { 4, s_1_111, 110, 1, 0},
/*112 */ { 3, s_1_112, -1, 1, 0},
/*113 */ { 3, s_1_113, -1, 1, 0},
/*114 */ { 4, s_1_114, 113, 1, 0},
/*115 */ { 3, s_1_115, -1, 1, 0},
/*116 */ { 4, s_1_116, -1, 1, 0},
/*117 */ { 3, s_1_117, -1, 1, 0},
/*118 */ { 4, s_1_118, -1, 1, 0},
/*119 */ { 3, s_1_119, -1, 1, 0},
/*120 */ { 4, s_1_120, -1, 1, 0},
/*121 */ { 2, s_1_121, -1, 1, 0},
/*122 */ { 4, s_1_122, 121, 1, 0},
/*123 */ { 2, s_1_123, -1, 1, 0},
/*124 */ { 4, s_1_124, 123, 1, 0},
/*125 */ { 2, s_1_125, -1, 1, 0},
/*126 */ { 2, s_1_126, -1, 1, 0},
/*127 */ { 4, s_1_127, 126, 1, 0},
/*128 */ { 4, s_1_128, 126, 1, 0},
/*129 */ { 4, s_1_129, 126, 1, 0},
/*130 */ { 2, s_1_130, -1, 1, 0},
/*131 */ { 4, s_1_131, 130, 1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 17, 12, 18 };


static int r_mark_regions(struct SN_env * z) {
    z->I[0] = z->l;
    z->I[1] = z->l;
    {   int c1 = z->c; /* do, line 41 */
        {    /* gopast */ /* non v, line 42 */
            int ret = in_grouping_U(z, g_v, 97, 253, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        z->I[0] = z->c; /* setmark pV, line 42 */
        {    /* gopast */ /* non v, line 43 */
            int ret = in_grouping_U(z, g_v, 97, 253, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {    /* gopast */ /* grouping v, line 43 */
            int ret = out_grouping_U(z, g_v, 97, 253, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        z->I[1] = z->c; /* setmark p1, line 43 */
    lab0:
        z->c = c1;
    }
    return 1;
}

static int r_RV(struct SN_env * z) {
    if (!(z->I[0] <= z->c)) return 0;
    return 1;
}

static int r_do_possessive(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 52 */
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 110 && z->p[z->c - 1] != 118)) return 0;
    among_var = find_among_b(z, a_0, 2); /* substring, line 52 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 52 */
    {   int ret = r_RV(z);
        if (ret == 0) return 0; /* call RV, line 52 */
        if (ret < 0) return ret;
    }
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 54 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 56 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_do_case(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 61 */
    among_var = find_among_b(z, a_1, 132); /* substring, line 61 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 61 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int ret = slice_del(z); /* delete, line 76 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int slovak_UTF_8_stem(struct SN_env * z) {
    {   int c1 = z->c; /* do, line 82 */
        {   int ret = r_mark_regions(z);
            if (ret == 0) goto lab0; /* call mark_regions, line 82 */
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    z->lb = z->c; z->c = z->l; /* backwards, line 83 */

    {   int ret = r_do_case(z);
        if (ret == 0) return 0; /* call do_case, line 84 */
        if (ret < 0) return ret;
    }
    {   int ret = r_do_possessive(z);
        if (ret == 0) return 0; /* call do_possessive, line 85 */
        if (ret < 0) return ret;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * slovak_UTF_8_create_env(void) { return SN_create_env(0, 2, 0); }

extern void slovak_UTF_8_close_env(struct SN_env * z) { SN_close_env(z, 0); }

