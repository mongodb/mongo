
/* This file was generated automatically by the Snowball to ANSI C compiler */

#include "../runtime/header.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int german_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif
static int r_standard_suffix(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_postlude(struct SN_env * z);
static int r_prelude(struct SN_env * z);
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * german_UTF_8_create_env(void);
extern void german_UTF_8_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_1[1] = { 'U' };
static const symbol s_0_2[1] = { 'Y' };
static const symbol s_0_3[2] = { 0xC3, 0xA4 };
static const symbol s_0_4[2] = { 0xC3, 0xB6 };
static const symbol s_0_5[2] = { 0xC3, 0xBC };

static const struct among a_0[6] =
{
/*  0 */ { 0, 0, -1, 6, 0},
/*  1 */ { 1, s_0_1, 0, 2, 0},
/*  2 */ { 1, s_0_2, 0, 1, 0},
/*  3 */ { 2, s_0_3, 0, 3, 0},
/*  4 */ { 2, s_0_4, 0, 4, 0},
/*  5 */ { 2, s_0_5, 0, 5, 0}
};

static const symbol s_1_0[1] = { 'e' };
static const symbol s_1_1[2] = { 'e', 'm' };
static const symbol s_1_2[2] = { 'e', 'n' };
static const symbol s_1_3[3] = { 'e', 'r', 'n' };
static const symbol s_1_4[2] = { 'e', 'r' };
static const symbol s_1_5[1] = { 's' };
static const symbol s_1_6[2] = { 'e', 's' };

static const struct among a_1[7] =
{
/*  0 */ { 1, s_1_0, -1, 2, 0},
/*  1 */ { 2, s_1_1, -1, 1, 0},
/*  2 */ { 2, s_1_2, -1, 2, 0},
/*  3 */ { 3, s_1_3, -1, 1, 0},
/*  4 */ { 2, s_1_4, -1, 1, 0},
/*  5 */ { 1, s_1_5, -1, 3, 0},
/*  6 */ { 2, s_1_6, 5, 2, 0}
};

static const symbol s_2_0[2] = { 'e', 'n' };
static const symbol s_2_1[2] = { 'e', 'r' };
static const symbol s_2_2[2] = { 's', 't' };
static const symbol s_2_3[3] = { 'e', 's', 't' };

static const struct among a_2[4] =
{
/*  0 */ { 2, s_2_0, -1, 1, 0},
/*  1 */ { 2, s_2_1, -1, 1, 0},
/*  2 */ { 2, s_2_2, -1, 2, 0},
/*  3 */ { 3, s_2_3, 2, 1, 0}
};

static const symbol s_3_0[2] = { 'i', 'g' };
static const symbol s_3_1[4] = { 'l', 'i', 'c', 'h' };

static const struct among a_3[2] =
{
/*  0 */ { 2, s_3_0, -1, 1, 0},
/*  1 */ { 4, s_3_1, -1, 1, 0}
};

static const symbol s_4_0[3] = { 'e', 'n', 'd' };
static const symbol s_4_1[2] = { 'i', 'g' };
static const symbol s_4_2[3] = { 'u', 'n', 'g' };
static const symbol s_4_3[4] = { 'l', 'i', 'c', 'h' };
static const symbol s_4_4[4] = { 'i', 's', 'c', 'h' };
static const symbol s_4_5[2] = { 'i', 'k' };
static const symbol s_4_6[4] = { 'h', 'e', 'i', 't' };
static const symbol s_4_7[4] = { 'k', 'e', 'i', 't' };

static const struct among a_4[8] =
{
/*  0 */ { 3, s_4_0, -1, 1, 0},
/*  1 */ { 2, s_4_1, -1, 2, 0},
/*  2 */ { 3, s_4_2, -1, 1, 0},
/*  3 */ { 4, s_4_3, -1, 3, 0},
/*  4 */ { 4, s_4_4, -1, 2, 0},
/*  5 */ { 2, s_4_5, -1, 2, 0},
/*  6 */ { 4, s_4_6, -1, 3, 0},
/*  7 */ { 4, s_4_7, -1, 4, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32, 8 };

static const unsigned char g_s_ending[] = { 117, 30, 5 };

static const unsigned char g_st_ending[] = { 117, 30, 4 };

static const symbol s_0[] = { 0xC3, 0x9F };
static const symbol s_1[] = { 's', 's' };
static const symbol s_2[] = { 'u' };
static const symbol s_3[] = { 'U' };
static const symbol s_4[] = { 'y' };
static const symbol s_5[] = { 'Y' };
static const symbol s_6[] = { 'y' };
static const symbol s_7[] = { 'u' };
static const symbol s_8[] = { 'a' };
static const symbol s_9[] = { 'o' };
static const symbol s_10[] = { 'u' };
static const symbol s_11[] = { 's' };
static const symbol s_12[] = { 'n', 'i', 's' };
static const symbol s_13[] = { 'i', 'g' };
static const symbol s_14[] = { 'e' };
static const symbol s_15[] = { 'e' };
static const symbol s_16[] = { 'e', 'r' };
static const symbol s_17[] = { 'e', 'n' };

static int r_prelude(struct SN_env * z) {
    {   int c_test = z->c; /* test, line 35 */
        while(1) { /* repeat, line 35 */
            int c1 = z->c;
            {   int c2 = z->c; /* or, line 38 */
                z->bra = z->c; /* [, line 37 */
                if (!(eq_s(z, 2, s_0))) goto lab2;
                z->ket = z->c; /* ], line 37 */
                {   int ret = slice_from_s(z, 2, s_1); /* <-, line 37 */
                    if (ret < 0) return ret;
                }
                goto lab1;
            lab2:
                z->c = c2;
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret; /* next, line 38 */
                }
            }
        lab1:
            continue;
        lab0:
            z->c = c1;
            break;
        }
        z->c = c_test;
    }
    while(1) { /* repeat, line 41 */
        int c3 = z->c;
        while(1) { /* goto, line 41 */
            int c4 = z->c;
            if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab4;
            z->bra = z->c; /* [, line 42 */
            {   int c5 = z->c; /* or, line 42 */
                if (!(eq_s(z, 1, s_2))) goto lab6;
                z->ket = z->c; /* ], line 42 */
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab6;
                {   int ret = slice_from_s(z, 1, s_3); /* <-, line 42 */
                    if (ret < 0) return ret;
                }
                goto lab5;
            lab6:
                z->c = c5;
                if (!(eq_s(z, 1, s_4))) goto lab4;
                z->ket = z->c; /* ], line 43 */
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab4;
                {   int ret = slice_from_s(z, 1, s_5); /* <-, line 43 */
                    if (ret < 0) return ret;
                }
            }
        lab5:
            z->c = c4;
            break;
        lab4:
            z->c = c4;
            {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                if (ret < 0) goto lab3;
                z->c = ret; /* goto, line 41 */
            }
        }
        continue;
    lab3:
        z->c = c3;
        break;
    }
    return 1;
}

static int r_mark_regions(struct SN_env * z) {
    z->I[0] = z->l;
    z->I[1] = z->l;
    {   int c_test = z->c; /* test, line 52 */
        {   int ret = skip_utf8(z->p, z->c, 0, z->l, + 3);
            if (ret < 0) return 0;
            z->c = ret; /* hop, line 52 */
        }
        z->I[2] = z->c; /* setmark x, line 52 */
        z->c = c_test;
    }
    {    /* gopast */ /* grouping v, line 54 */
        int ret = out_grouping_U(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {    /* gopast */ /* non v, line 54 */
        int ret = in_grouping_U(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[0] = z->c; /* setmark p1, line 54 */
     /* try, line 55 */
    if (!(z->I[0] < z->I[2])) goto lab0;
    z->I[0] = z->I[2];
lab0:
    {    /* gopast */ /* grouping v, line 56 */
        int ret = out_grouping_U(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {    /* gopast */ /* non v, line 56 */
        int ret = in_grouping_U(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[1] = z->c; /* setmark p2, line 56 */
    return 1;
}

static int r_postlude(struct SN_env * z) {
    int among_var;
    while(1) { /* repeat, line 60 */
        int c1 = z->c;
        z->bra = z->c; /* [, line 62 */
        among_var = find_among(z, a_0, 6); /* substring, line 62 */
        if (!(among_var)) goto lab0;
        z->ket = z->c; /* ], line 62 */
        switch(among_var) {
            case 0: goto lab0;
            case 1:
                {   int ret = slice_from_s(z, 1, s_6); /* <-, line 63 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_from_s(z, 1, s_7); /* <-, line 64 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {   int ret = slice_from_s(z, 1, s_8); /* <-, line 65 */
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {   int ret = slice_from_s(z, 1, s_9); /* <-, line 66 */
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {   int ret = slice_from_s(z, 1, s_10); /* <-, line 67 */
                    if (ret < 0) return ret;
                }
                break;
            case 6:
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret; /* next, line 68 */
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

static int r_R1(struct SN_env * z) {
    if (!(z->I[0] <= z->c)) return 0;
    return 1;
}

static int r_R2(struct SN_env * z) {
    if (!(z->I[1] <= z->c)) return 0;
    return 1;
}

static int r_standard_suffix(struct SN_env * z) {
    int among_var;
    {   int m1 = z->l - z->c; (void)m1; /* do, line 79 */
        z->ket = z->c; /* [, line 80 */
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((811040 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0;
        among_var = find_among_b(z, a_1, 7); /* substring, line 80 */
        if (!(among_var)) goto lab0;
        z->bra = z->c; /* ], line 80 */
        {   int ret = r_R1(z);
            if (ret == 0) goto lab0; /* call R1, line 80 */
            if (ret < 0) return ret;
        }
        switch(among_var) {
            case 0: goto lab0;
            case 1:
                {   int ret = slice_del(z); /* delete, line 82 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_del(z); /* delete, line 85 */
                    if (ret < 0) return ret;
                }
                {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 86 */
                    z->ket = z->c; /* [, line 86 */
                    if (!(eq_s_b(z, 1, s_11))) { z->c = z->l - m_keep; goto lab1; }
                    z->bra = z->c; /* ], line 86 */
                    if (!(eq_s_b(z, 3, s_12))) { z->c = z->l - m_keep; goto lab1; }
                    {   int ret = slice_del(z); /* delete, line 86 */
                        if (ret < 0) return ret;
                    }
                lab1:
                    ;
                }
                break;
            case 3:
                if (in_grouping_b_U(z, g_s_ending, 98, 116, 0)) goto lab0;
                {   int ret = slice_del(z); /* delete, line 89 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        z->c = z->l - m1;
    }
    {   int m2 = z->l - z->c; (void)m2; /* do, line 93 */
        z->ket = z->c; /* [, line 94 */
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1327104 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab2;
        among_var = find_among_b(z, a_2, 4); /* substring, line 94 */
        if (!(among_var)) goto lab2;
        z->bra = z->c; /* ], line 94 */
        {   int ret = r_R1(z);
            if (ret == 0) goto lab2; /* call R1, line 94 */
            if (ret < 0) return ret;
        }
        switch(among_var) {
            case 0: goto lab2;
            case 1:
                {   int ret = slice_del(z); /* delete, line 96 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                if (in_grouping_b_U(z, g_st_ending, 98, 116, 0)) goto lab2;
                {   int ret = skip_utf8(z->p, z->c, z->lb, z->l, - 3);
                    if (ret < 0) goto lab2;
                    z->c = ret; /* hop, line 99 */
                }
                {   int ret = slice_del(z); /* delete, line 99 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab2:
        z->c = z->l - m2;
    }
    {   int m3 = z->l - z->c; (void)m3; /* do, line 103 */
        z->ket = z->c; /* [, line 104 */
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1051024 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab3;
        among_var = find_among_b(z, a_4, 8); /* substring, line 104 */
        if (!(among_var)) goto lab3;
        z->bra = z->c; /* ], line 104 */
        {   int ret = r_R2(z);
            if (ret == 0) goto lab3; /* call R2, line 104 */
            if (ret < 0) return ret;
        }
        switch(among_var) {
            case 0: goto lab3;
            case 1:
                {   int ret = slice_del(z); /* delete, line 106 */
                    if (ret < 0) return ret;
                }
                {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 107 */
                    z->ket = z->c; /* [, line 107 */
                    if (!(eq_s_b(z, 2, s_13))) { z->c = z->l - m_keep; goto lab4; }
                    z->bra = z->c; /* ], line 107 */
                    {   int m4 = z->l - z->c; (void)m4; /* not, line 107 */
                        if (!(eq_s_b(z, 1, s_14))) goto lab5;
                        { z->c = z->l - m_keep; goto lab4; }
                    lab5:
                        z->c = z->l - m4;
                    }
                    {   int ret = r_R2(z);
                        if (ret == 0) { z->c = z->l - m_keep; goto lab4; } /* call R2, line 107 */
                        if (ret < 0) return ret;
                    }
                    {   int ret = slice_del(z); /* delete, line 107 */
                        if (ret < 0) return ret;
                    }
                lab4:
                    ;
                }
                break;
            case 2:
                {   int m5 = z->l - z->c; (void)m5; /* not, line 110 */
                    if (!(eq_s_b(z, 1, s_15))) goto lab6;
                    goto lab3;
                lab6:
                    z->c = z->l - m5;
                }
                {   int ret = slice_del(z); /* delete, line 110 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {   int ret = slice_del(z); /* delete, line 113 */
                    if (ret < 0) return ret;
                }
                {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 114 */
                    z->ket = z->c; /* [, line 115 */
                    {   int m6 = z->l - z->c; (void)m6; /* or, line 115 */
                        if (!(eq_s_b(z, 2, s_16))) goto lab9;
                        goto lab8;
                    lab9:
                        z->c = z->l - m6;
                        if (!(eq_s_b(z, 2, s_17))) { z->c = z->l - m_keep; goto lab7; }
                    }
                lab8:
                    z->bra = z->c; /* ], line 115 */
                    {   int ret = r_R1(z);
                        if (ret == 0) { z->c = z->l - m_keep; goto lab7; } /* call R1, line 115 */
                        if (ret < 0) return ret;
                    }
                    {   int ret = slice_del(z); /* delete, line 115 */
                        if (ret < 0) return ret;
                    }
                lab7:
                    ;
                }
                break;
            case 4:
                {   int ret = slice_del(z); /* delete, line 119 */
                    if (ret < 0) return ret;
                }
                {   int m_keep = z->l - z->c;/* (void) m_keep;*/ /* try, line 120 */
                    z->ket = z->c; /* [, line 121 */
                    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 103 && z->p[z->c - 1] != 104)) { z->c = z->l - m_keep; goto lab10; }
                    among_var = find_among_b(z, a_3, 2); /* substring, line 121 */
                    if (!(among_var)) { z->c = z->l - m_keep; goto lab10; }
                    z->bra = z->c; /* ], line 121 */
                    {   int ret = r_R2(z);
                        if (ret == 0) { z->c = z->l - m_keep; goto lab10; } /* call R2, line 121 */
                        if (ret < 0) return ret;
                    }
                    switch(among_var) {
                        case 0: { z->c = z->l - m_keep; goto lab10; }
                        case 1:
                            {   int ret = slice_del(z); /* delete, line 123 */
                                if (ret < 0) return ret;
                            }
                            break;
                    }
                lab10:
                    ;
                }
                break;
        }
    lab3:
        z->c = z->l - m3;
    }
    return 1;
}

extern int german_UTF_8_stem(struct SN_env * z) {
    {   int c1 = z->c; /* do, line 134 */
        {   int ret = r_prelude(z);
            if (ret == 0) goto lab0; /* call prelude, line 134 */
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    {   int c2 = z->c; /* do, line 135 */
        {   int ret = r_mark_regions(z);
            if (ret == 0) goto lab1; /* call mark_regions, line 135 */
            if (ret < 0) return ret;
        }
    lab1:
        z->c = c2;
    }
    z->lb = z->c; z->c = z->l; /* backwards, line 136 */

    {   int m3 = z->l - z->c; (void)m3; /* do, line 137 */
        {   int ret = r_standard_suffix(z);
            if (ret == 0) goto lab2; /* call standard_suffix, line 137 */
            if (ret < 0) return ret;
        }
    lab2:
        z->c = z->l - m3;
    }
    z->c = z->lb;
    {   int c4 = z->c; /* do, line 138 */
        {   int ret = r_postlude(z);
            if (ret == 0) goto lab3; /* call postlude, line 138 */
            if (ret < 0) return ret;
        }
    lab3:
        z->c = c4;
    }
    return 1;
}

extern struct SN_env * german_UTF_8_create_env(void) { return SN_create_env(0, 3, 0); }

extern void german_UTF_8_close_env(struct SN_env * z) { SN_close_env(z, 0); }

