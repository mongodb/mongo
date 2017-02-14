/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "wt_internal.h"			/* For __wt_XXX */

#ifdef _WIN32
	#define DIR_DELIM '\\'
	#define DIR_DELIM_STR "\\"
	#define DIR_EXISTS_COMMAND "IF EXIST "
	#define RM_COMMAND "rd /s /q "
#else
	#define	DIR_DELIM '/'
	#define	DIR_DELIM_STR "/"
	#define RM_COMMAND "rm -rf "
#endif

#define	DEFAULT_DIR "WT_TEST"
#define	MKDIR_COMMAND "mkdir "

#ifdef _WIN32
#include "windows_shim.h"
#endif

/* Generic option parsing structure shared by all test cases. */
typedef struct {
	char  *home;
	char  *progname;
	enum {	TABLE_COL=1,	/* Fixed-length column store */
		TABLE_FIX=2,	/* Variable-length column store */
		TABLE_ROW=3	/* Row-store */
	} table_type;
	bool	     preserve;			/* Don't remove files on exit */
	bool	     verbose;			/* Run in verbose mode */
	uint64_t     nrecords;			/* Number of records */
	uint64_t     nops;			/* Number of operations */
	uint64_t     nthreads;			/* Number of threads */
	uint64_t     n_append_threads;		/* Number of append threads */
	uint64_t     n_read_threads;		/* Number of read threads */
	uint64_t     n_write_threads;		/* Number of write threads */

	/*
	 * Fields commonly shared within a test program. The test cleanup
	 * function will attempt to automatically free and close non-null
	 * resources.
	 */
	WT_CONNECTION *conn;
	WT_SESSION    *session;
	bool	   running;
	char	  *uri;
	volatile uint64_t   next_threadid;
	uint64_t   max_inserted_id;
} TEST_OPTS;

/*
 * testutil_assert --
 *	Complain and quit if something isn't true.
 */
#define	testutil_assert(a) do {						\
	if (!(a))							\
		testutil_die(0, "%s/%d: %s", __func__, __LINE__, #a);	\
} while (0)

/*
 * testutil_assertfmt --
 *	Complain and quit if something isn't true.
 */
#define	testutil_assertfmt(a, fmt, ...) do {				\
	if (!(a))							\
		testutil_die(0, "%s/%d: %s: " fmt,			\
		__func__, __LINE__, #a, __VA_ARGS__);			\
} while (0)

/*
 * testutil_check --
 *	Complain and quit if a function call fails.
 */
#define	testutil_check(call) do {					\
	int __r;							\
	if ((__r = (call)) != 0)					\
		testutil_die(						\
		    __r, "%s/%d: %s", __func__, __LINE__, #call);	\
} while (0)

/*
 * testutil_checkfmt --
 *	Complain and quit if a function call fails, with additional arguments.
 */
#define	testutil_checkfmt(call, fmt, ...) do {				\
	int __r;							\
	if ((__r = (call)) != 0)					\
		testutil_die(__r, "%s/%d: %s: " fmt,			\
		    __func__, __LINE__, #call, __VA_ARGS__);		\
} while (0)

/*
 * u64_to_string --
 *	Convert a uint64_t to a text string.
 *
 * Algorithm from Andrei Alexandrescu's talk: "Three Optimization Tips for C++"
 */
static inline void
u64_to_string(uint64_t n, char **pp)
{
	static const char hundred_lookup[201] =
	    "0001020304050607080910111213141516171819"
	    "2021222324252627282930313233343536373839"
	    "4041424344454647484950515253545556575859"
	    "6061626364656667686970717273747576777879"
	    "8081828384858687888990919293949596979899";
	u_int i;
	char *p;

	/*
	 * The argument pointer references the last element of a buffer (which
	 * must be large enough to hold any possible value).
	 *
	 * Nul-terminate the buffer.
	 */
	for (p = *pp, *p-- = '\0'; n >= 100; n /= 100) {
		i = (n % 100) * 2;
		*p-- = hundred_lookup[i + 1];
		*p-- = hundred_lookup[i];
	}

	/* Handle the last two digits. */
	i = (u_int)n * 2;
	*p = hundred_lookup[i + 1];
	if (n >= 10)
		*--p = hundred_lookup[i];

	/* Return a pointer to the first byte of the text string. */
	*pp = p;
}

/*
 * u64_to_string_zf --
 *	Convert a uint64_t to a text string, zero-filling the buffer.
 */
static inline void
u64_to_string_zf(uint64_t n, char *buf, size_t len)
{
	char *p;

	p = buf + (len - 1);
	u64_to_string(n, &p);

	while (p > buf)
		*--p = '0';
}

/* Allow tests to add their own death handling. */
extern void (*custom_die)(void);

void testutil_die(int, const char *, ...)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

void *dcalloc(size_t, size_t);
void *dmalloc(size_t);
void *drealloc(void *, size_t);
void *dstrdup(const void *);
void *dstrndup(const char *, size_t);
void  testutil_clean_work_dir(const char *);
void  testutil_cleanup(TEST_OPTS *);
bool  testutil_disable_long_tests(void);
void  testutil_make_work_dir(char *);
int   testutil_parse_opts(int, char * const *, TEST_OPTS *);
void  testutil_work_dir_from_path(char *, size_t, const char *);
void *thread_append(void *);
void *thread_insert_append(void *);
void *thread_prev(void *);
