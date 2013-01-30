
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
    
    deleteAll = False

    for ( key , modify , etag , size ) in bucket.listdir( prefix=prefix ):
        if key.find( todel ) < 0:
            continue
        print( key )

        if not deleteAll:

            val = raw_input( "Delete (Y,y,n,N):" ).strip()

            if val == "n":
                print( "skipping this one" )
                continue
            elif val == "N":
                break

            if val == "Y":
                val = "y"
                deleteAll = True
                
            if val != "y":
                raise Exception( "invalid input :(" )

        bucket.delete( key )

def clean( todel ):


    bucket = simples3.S3Bucket( settings.bucket , settings.id , settings.key )
    
    for x in [ "osx" , "linux" , "win32" , "sunos5" , "src" ]:
        check_dir( bucket , x , todel )
    

if __name__ == "__main__":
    clean( sys.argv[1] )
