/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once
#ifdef _WIN32
#ifndef _WIN32_WINNT
#if defined _WIN64
// For 64 bit Windows we require at least Windows Vista or Windows Server 2008.
#define _WIN32_WINNT 0x0600
#else
// For 32 bit Windows we allow Windows XP as well. See platform/atomic_intrinsics_win32.h for
// an example of where this makes a difference.
#define _WIN32_WINNT 0x0502
#endif
#endif
#endif
