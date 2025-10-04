
# Copyright Aleksey Gurtovoy 2001-2004
#
# Distributed under the Boost Software License, Version 1.0. 
# (See accompanying file LICENSE_1_0.txt or copy at 
# http://www.boost.org/LICENSE_1_0.txt)
#
# See http://www.boost.org/libs/mpl for documentation.

# $Id$
# $Date$
# $Revision$

import pp
import shutil
import os.path
import os
import string
import sys

preprocess_cmd = open( "preprocess.cmd" ).readlines()[0]

def process( file, boost_root, dst_dir, mode ):
    file_path = "%s.hpp" % os.path.splitext( file )[0]
    os.system( preprocess_cmd % {
          'boost_root': boost_root
        , 'mode': mode
        , 'file': file
        , 'file_path': file_path
        } )

    os.rename( file_path, "%s.tmp" % file_path )
    pp.main( "%s.tmp" % file_path, file_path )
    os.remove( "%s.tmp" % file_path )

    filename = os.path.basename(file_path)
    dst_dir = os.path.join( dst_dir, mode )
    dst_file = os.path.join( dst_dir, filename )

    if os.path.exists( dst_file ):
        shutil.copymode( filename, dst_file )
        
    shutil.copy( filename, dst_dir )
    os.remove( filename )


def process_all( root, boost_root, dst_dir, mode ):
    files = os.listdir( root )
    for file in files:
        path = os.path.join( root, file )
        if os.path.splitext( file )[1] == ".cpp":
            process( path, boost_root, dst_dir, mode )
        else:
            if os.path.isdir( path ):
                process_all( path, boost_root, dst_dir, mode )


def main( all_modes, src_dir, dst_dir ):
    if len( sys.argv ) < 2:
        print "\nUsage:\n\t %s <mode> <boost_root> [<source_file>]" % os.path.basename( sys.argv[0] )
        print "\nPurpose:\n\t updates preprocessed version(s) of the header(s) in \"%s\" directory" % dst_dir
        print "\nExample:\n\t the following command will re-generate and update all 'apply.hpp' headers:"
        print "\n\t\t %s all f:\\cvs\\boost apply.cpp" % os.path.basename( sys.argv[0] )
        sys.exit( -1 )

    if sys.argv[1] == "all":
        modes = all_modes
    else:
        modes = [sys.argv[1]]

    boost_root = sys.argv[2]
    dst_dir = os.path.join( boost_root, dst_dir )
    
    for mode in modes:
        if len( sys.argv ) > 3:
            file = os.path.join( os.path.join( os.getcwd(), src_dir ), sys.argv[3] )
            process( file, boost_root, dst_dir, mode )
        else:
            process_all( os.path.join( os.getcwd(), src_dir ), boost_root, dst_dir, mode )


if __name__ == '__main__':
    
    main(
          ["bcc", "bcc551", "gcc", "mwcw", "dmc", "no_ttp", "plain"]
        , "src"
        , os.path.join( "boost", "mpl", "aux_", "preprocessed" )
        )
