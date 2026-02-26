#ifndef __snprintf_compat_h
#define __snprintf_compat_h

/**
 * @file
 * @brief Do not use, json-c internal, may be changed or removed at any time.
 */

/*
 * Microsoft's _vsnprintf and _snprint don't always terminate
 * the string, so use wrappers that ensure that.
 */

#include <stdarg.h>

#if !defined(HAVE_SNPRINTF) && (defined(_MSC_VER) || defined(__MINGW32__))
static int json_c_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	int ret;
	ret = _vsnprintf(str, size, format, ap);
	str[size - 1] = '\0';
	return ret;
}
#define vsnprintf json_c_vsnprintf

static int json_c_snprintf(char *str, size_t size, const char *format, ...)
{
	va_list ap;
	int ret;
	va_start(ap, format);
	ret = json_c_vsnprintf(str, size, format, ap);
	va_end(ap);
	return ret;
}
#define snprintf json_c_snprintf

#elif !defined(HAVE_SNPRINTF) /* !HAVE_SNPRINTF */
#error snprintf is required but was not found
#endif /* !HAVE_SNPRINTF && defined(WIN32) */

#endif /* __snprintf_compat_h */
