/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#undef vsnprintf

_Check_return_opt_ int __cdecl _wt_vsnprintf(
    _Out_writes_(_MaxCount) char * _DstBuf,
    _In_ size_t _MaxCount,
    _In_z_ _Printf_format_string_ const char * _Format,
    va_list _ArgList)
{
	int len;

	len = (size_t)vsnprintf(_DstBuf, _MaxCount, _Format, _ArgList);

	/*
	 * The MSVC implementation returns -1 on truncation instead of what
	 * it would have written.  We could iteratively grow the buffer, or
	 * just ask us how big a buffer they would like.
	 */
	if (len == -1)
		len = _vscprintf(_Format, _ArgList) + 1;

	return (len);
}
