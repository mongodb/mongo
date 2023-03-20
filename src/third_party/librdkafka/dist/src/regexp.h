/**
 * Copyright: public domain
 *
 * From https://github.com/ccxvii/minilibs sha 875c33568b5a4aa4fb3dd0c52ea98f7f0e5ca684:
 *
 * These libraries are in the public domain (or the equivalent where that is not possible).
 * You can do anything you want with them. You have no legal obligation to do anything else,
 * although I appreciate attribution.
 */

#ifndef regexp_h
#define regexp_h

typedef struct Reprog Reprog;
typedef struct Resub Resub;

Reprog *re_regcomp(const char *pattern, int cflags, const char **errorp);
int re_regexec(Reprog *prog, const char *string, Resub *sub, int eflags);
void re_regfree(Reprog *prog);

enum {
	/* regcomp flags */
	REG_ICASE = 1,
	REG_NEWLINE = 2,

	/* regexec flags */
	REG_NOTBOL = 4,

	/* limits */
	REG_MAXSUB = 16
};

struct Resub {
	unsigned int nsub;
	struct {
		const char *sp;
		const char *ep;
	} sub[REG_MAXSUB];
};

#endif
