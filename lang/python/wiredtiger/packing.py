#!/usr/bin/env python
#
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# WiredTiger variable-length packing and unpacking functions

from intpacking import pack_int, unpack_int

def __get_type(fmt):
	if not fmt:
		return None, fmt
	# Variable-sized encoding is the default (and only supported format in v1)
	if fmt[0] in '.@<>':
		tfmt = fmt[0]
		fmt = fmt[1:]
	else:
		tfmt = '.'
	return tfmt, fmt

def unpack(fmt, s):
	tfmt, fmt = __get_type(fmt)
	if not fmt:
		return ()
	if tfmt != '.':
		raise ValueError('Only variable-length encoding is currently supported')
	result = []
	havesize = size = 0
	for offset, f in enumerate(fmt):
		if f.isdigit():
			size = (size * 10) + int(f)
			havesize = 1
			continue
		elif f == 'x':
			if not havesize:
				size = 1
			s = s[size:]
			# Note: no value, don't increment i
		elif f in 'Ssu':
			if not havesize:
				if f == 's':
					size = 1
				elif f == 'S':
					size = s.find('\0')
				elif f == 'u':
					if offset == len(fmt) - 1:
						size = len(s)
					else:
						size, s = unpack_int(s)
			result.append(s[:size])
			if f == 'S' and not havesize:
				size += 1
			s = s[size:]
		elif f in 't':
			# bit type, size is number of bits
			if not havesize:
				size = 1
			result.append(ord(s[0:1]))
			s = s[1:]
		else:
			# integral type
			if not havesize:
				size = 1
			for j in xrange(size):
				v, s = unpack_int(s)
				result.append(v)
		havesize = size = 0
	return result

def pack(fmt, *values):
	tfmt, fmt = __get_type(fmt)
	if not fmt:
		return ()
	if tfmt != '.':
		raise ValueError('Only variable-length encoding is currently supported')
	result = ''
	havesize = i = size = 0
	for offset, f in enumerate(fmt):
		if f.isdigit():
			size = (size * 10) + int(f)
			havesize = 1
			continue
		elif f == 'x':
			if not havesize:
				result += '\0'
			else:
				result += '\0' * size
			# Note: no value, don't increment i
		elif f in 'Ssu':
			val = values[i]
			if f == 'S' and '\0' in val:
				l = val.find('\0')
			else:
				l = len(val)
			if havesize:
				if l > size:
					l = size
			elif f == 's':
				havesize = size = 1
			elif f == 'u' and offset != len(fmt) - 1:
				result += pack_int(l)
			if type(val) is unicode and f in 'Ss':
				result += str(val[:l])
			else:
				result += val[:l]
			if f == 'S' and not havesize:
				result += '\0'
			elif size > l:
				result += '\0' * (size - l)
			i += 1
		elif f in 't':
			# bit type, size is number of bits
			if not havesize:
				size = 1
                        if size > 8:
				raise ValueError("bit count cannot be greater than 8 for 't' encoding")
			mask = (1 << size) - 1
			val = values[i]
                        if (mask & val) != val:
				raise ValueError("value out of range for 't' encoding")
			result += chr(val)
			i += 1
		else:
			# integral type
			if not havesize:
				size = 1
			for j in xrange(size):
				result += pack_int(values[i])
				i += 1
		havesize = size = 0
	return result
