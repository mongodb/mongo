/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

_Check_return_opt_ int __cdecl _wt_vsnprintf(
    _Out_writes_(_MaxCount) char * _DstBuf,
    _In_ size_t _MaxCount,
    _In_z_ _Printf_format_string_ const char * _Format,
    va_list _ArgList)
{
	int len;

	/*
	 * WiredTiger will call with length 0 to get the needed buffer size
	 * We call the count only version in this case since vsnprintf_s assumes
	 * length is greater than zero or else it triggers the invalid_parameter
	 * handler.
	 */
	if (_MaxCount == 0) {
		return _vscprintf(_Format, _ArgList);
	}

	len = (size_t)_vsnprintf_s(
		_DstBuf, _MaxCount, _TRUNCATE, _Format, _ArgList);

	/*
	 * The MSVC implementation returns -1 on truncation instead of what
	 * it would have written.  We could let callers iteratively grow the
	 * buffer, or just ask us how big a buffer they would like.
	 */
	if (len == -1)
		len = _vscprintf(_Format, _ArgList) + 1;

	return (len);
}
