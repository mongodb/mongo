
# Copyright 2017 Peter Dimov.
#
# Distributed under the Boost Software License, Version 1.0.
#
# See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt

import os.path
import re
import sys

included = []

def scan_header( prefix, dir, fn ):

	path = os.path.join( prefix, dir, fn )

	if path in included:

		return

	included.append( path )

	with open( path, 'r' ) as header:

		for line in header:

			m = re.match( '[ \t]*#[ \t]*include[ \t]*(["<])([^">]*)[">]', line )

			r = False

			if m:

				h = m.group( 2 )

				hfn1 = os.path.join( prefix, h )
				hfn2 = os.path.join( prefix, dir, h )

				if m.group( 1 ) == '"' and os.path.exists( hfn2 ):

					scan_header( prefix, os.path.join( dir, os.path.dirname( hfn2 ) ), os.path.basename( hfn2 ) )
					r = True

				elif os.path.exists( hfn1 ):

					scan_header( prefix, os.path.dirname( h ), os.path.basename( h ) )
					r = True

			if not r:

				sys.stdout.write( line )

scan_header( 'include', 'boost', 'mp11.hpp' )
