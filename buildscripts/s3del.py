
import os
import sys
import time

sys.path.append( "." )
sys.path.append( ".." )
sys.path.append( "../../" )
sys.path.append( "../../../" )

import simples3
import settings
import subprocess

# check s3 for md5 hashes

def check_dir( bucket , prefix , todel ):
    
    for ( key , modify , etag , size ) in bucket.listdir( prefix=prefix ):
        if key.find( todel ) < 0:
            continue
        print( key )
        time.sleep( 2 )
        bucket.delete( key )

def clean( todel ):


    bucket = simples3.S3Bucket( settings.bucket , settings.id , settings.key )
    
    for x in [ "osx" , "linux" , "win32" , "sunos5" , "src" ]:
        check_dir( bucket , x , todel )
    

if __name__ == "__main__":
    clean( sys.argv[1] )
