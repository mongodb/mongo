#!/usr/bin/env python

from intpacking import compress_int

i = 1
while i < 1 << 60:
	print -i, ''.join('%02x' % ord(c) for c in compress_int(-i))
	print i, ''.join('%02x' % ord(c) for c in compress_int(i))
	i <<= 1
