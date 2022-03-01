/*
Copyright (c) 1999 Samphan Raruenrom <samphan@thai.com>
Permission to use, copy, modify, distribute and sell this software
and its documentation for any purpose is hereby granted without fee,
provided that the above copyright notice appear in all copies and
that both that copyright notice and this permission notice appear
in supporting documentation.  Samphan Raruenrom makes no
representations about the suitability of this software for any
purpose.  It is provided "as is" without express or implied warranty.
*/
#ifndef __RULEBRK_H__
#define __RULEBRK_H__
#include "th_char.h"

#ifdef __cplusplus
extern "C" {
#endif

int TrbWordBreakPos(const th_char* pstr, int left, const th_char* rstr,
                    int right);
int TrbFollowing(const th_char* begin, int length, int offset);

#ifdef __cplusplus
}
#endif
#endif
