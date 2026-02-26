#ifndef __vasprintf_compat_h
#define __vasprintf_compat_h

/**
 * @file
 * @brief Do not use, json-c internal, may be changed or removed at any time.
 */

#include "snprintf_compat.h"

#ifndef WIN32
#include <stdarg.h>
#endif /* !defined(WIN32) */
#include <stdint.h>
#include <stdlib.h>

#if !defined(HAVE_VASPRINTF)
/* CAW: compliant version of vasprintf */
static int vasprintf(char **buf, const char *fmt, va_list ap)
{
#ifndef WIN32
	static char _T_emptybuffer = '\0';
	va_list ap2;
#endif /* !defined(WIN32) */
	int chars;
	char *b;

	if (!buf)
	{
		return -1;
	}

#ifdef WIN32
	chars = _vscprintf(fmt, ap);
#else  /* !defined(WIN32) */
	/* CAW: RAWR! We have to hope to god here that vsnprintf doesn't overwrite
	 * our buffer like on some 64bit sun systems... but hey, it's time to move on
	 */
	va_copy(ap2, ap);
	chars = vsnprintf(&_T_emptybuffer, 0, fmt, ap2);
	va_end(ap2);
#endif /* defined(WIN32) */
	if (chars < 0 || (size_t)chars + 1 > SIZE_MAX / sizeof(char))
	{
		return -1;
	}

	b = (char *)malloc(sizeof(char) * ((size_t)chars + 1));
	if (!b)
	{
		return -1;
	}

	if ((chars = vsprintf(b, fmt, ap)) < 0)
	{
		free(b);
	}
	else
	{
		*buf = b;
	}

	return chars;
}
#endif /* !HAVE_VASPRINTF */

#endif /* __vasprintf_compat_h */
