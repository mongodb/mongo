
import os
import glob

def insert( env , options ):
    jslibPaths = glob.glob('/usr/include/js-*/')
    if len(jslibPaths) >= 1:
        jslibPath = jslibPaths.pop()
        env.Append( CPPPATH=[ jslibPath ] )