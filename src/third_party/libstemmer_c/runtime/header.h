#pragma once

#include <limits.h>

#include "api.h"

#define MAXINT INT_MAX
#define MININT INT_MIN

#define HEAD 2*sizeof(int)

#define SIZE(p)        ((int *)(p))[-1]
#define SET_SIZE(p, n) ((int *)(p))[-1] = n
#define CAPACITY(p)    ((int *)(p))[-2]

// MONGO including utilities.c and marking all of its functions as static inline significantly
// improves stemmer perf. SERVER-19936
#ifdef _MSC_VER
#define SNOWBALL_INLINE static __inline
#else
#define SNOWBALL_INLINE static inline
#endif

struct among
{   int s_size;     /* number of chars in string */
    const symbol * s;       /* search string */
    int substring_i;/* index to longest matching substring */
    int result;     /* result of the lookup */
    int (* function)(struct SN_env *);
};

SNOWBALL_INLINE symbol * create_s(void);
SNOWBALL_INLINE void lose_s(symbol * p);

SNOWBALL_INLINE int skip_utf8(const symbol * p, int c, int lb, int l, int n);

SNOWBALL_INLINE int in_grouping_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
SNOWBALL_INLINE int in_grouping_b_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
SNOWBALL_INLINE int out_grouping_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
SNOWBALL_INLINE int out_grouping_b_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);

SNOWBALL_INLINE int in_grouping(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
SNOWBALL_INLINE int in_grouping_b(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
SNOWBALL_INLINE int out_grouping(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
SNOWBALL_INLINE int out_grouping_b(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);

SNOWBALL_INLINE int eq_s(struct SN_env * z, int s_size, const symbol * s);
SNOWBALL_INLINE int eq_s_b(struct SN_env * z, int s_size, const symbol * s);
SNOWBALL_INLINE int eq_v(struct SN_env * z, const symbol * p);
SNOWBALL_INLINE int eq_v_b(struct SN_env * z, const symbol * p);

SNOWBALL_INLINE int find_among(struct SN_env * z, const struct among * v, int v_size);
SNOWBALL_INLINE int find_among_b(struct SN_env * z, const struct among * v, int v_size);

SNOWBALL_INLINE int replace_s(struct SN_env * z, int c_bra, int c_ket, int s_size, const symbol * s, int * adjustment);
SNOWBALL_INLINE int slice_from_s(struct SN_env * z, int s_size, const symbol * s);
SNOWBALL_INLINE int slice_from_v(struct SN_env * z, const symbol * p);
SNOWBALL_INLINE int slice_del(struct SN_env * z);

SNOWBALL_INLINE int insert_s(struct SN_env * z, int bra, int ket, int s_size, const symbol * s);
SNOWBALL_INLINE int insert_v(struct SN_env * z, int bra, int ket, const symbol * p);

SNOWBALL_INLINE symbol * slice_to(struct SN_env * z, symbol * p);
SNOWBALL_INLINE symbol * assign_to(struct SN_env * z, symbol * p);

#if 0
static void debug(struct SN_env * z, int number, int line_count);
#endif
#include "utilities.c"

