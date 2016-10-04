/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

_Check_return_opt_ int __cdecl _wt_snprintf(
    _Out_writes_(_MaxCount) char * _DstBuf,
    _In_ size_t _MaxCount,
    _In_z_ _Printf_format_string_ const char * _Format, ...)
{
	va_list args;
	WT_DECL_RET;

	va_start(args, _Format);
	ret = _wt_vsnprintf(_DstBuf, _MaxCount, _Format, args);
	va_end(args);

	return (ret);
}
