
import utils
import os
import shutil
import sys

def go( boost_root ):
    
    OUTPUT = "src/third_party/boost"
    if os.path.exists( OUTPUT ):
        shutil.rmtree( OUTPUT )

    cmd = [ "bcp" , "--scan" , "--boost=%s" % boost_root ]
    
    src = utils.getAllSourceFiles()
    
    cmd += src
    cmd.append( OUTPUT )

    if not os.path.exists( OUTPUT ):
        os.makedirs( OUTPUT )

    res = utils.execsys( cmd )
    
    out = open( OUTPUT + "/bcp-out.txt" , 'w' )
    out.write( res[0] )
    out.close()
    
    out = open( OUTPUT + "/notes.txt" , 'w' )
    out.write( "command: " + " ".join( cmd ) )
    out.close()

    print( res[1] )

if __name__ == "__main__":
    if len(sys.argv) == 1:
        print( "usage: python %s <boost root directory>" % sys.argv[0] )
        sys.exit(1)
    go( sys.argv[1] )

