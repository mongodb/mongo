/*
** This file is in the public domain, so clarified as of
** 2006-07-17 by Arthur David Olson.
*/

/*LINTLIBRARY*/

#include "private.h"

char *
icatalloc(char *const old, const char *const new)
{
	register char *	result;
	register int	oldsize, newsize;

	newsize = (new == NULL) ? 0 : strlen(new);
	if (old == NULL)
		oldsize = 0;
	else if (newsize == 0)
		return old;
	else	oldsize = strlen(old);
	if ((result = realloc(old, oldsize + newsize + 1)) != NULL)
		if (new != NULL)
			(void) strcpy(result + oldsize, new);
	return result;
}

char *
icpyalloc(const char *const string)
{
	return icatalloc(NULL, string);
}
