
import os
import sys

sys.path.append( "." )
sys.path.append( ".." )
sys.path.append( "../../" )
sys.path.append( "../../../" )

import simples3
import settings
import subprocess

# check s3 for md5 hashes

def check_dir( bucket , prefix ):
    
    zips = {}
    md5s = {}
    for ( key , modify , etag , size ) in bucket.listdir( prefix=prefix ):
        if key.endswith( ".tgz" ) or key.endswith( ".zip" ) or key.endswith( ".tar.gz" ):
            zips[key] = etag.replace( '"' , '' )
        elif key.endswith( ".md5" ):
            md5s[key] = True
        elif key.find( "$folder$" ) > 0:
            pass
        else:
            print( "unknown file type: " + key )
            
    for x in zips:
        m = x + ".md5"
        if m in md5s:
            continue

        print( "need to do: " + x + " " + zips[x] + " to " + m )
        bucket.put( m , zips[x] , acl="public-read" )
        

def run():

    bucket = simples3.S3Bucket( settings.bucket , settings.id , settings.key )
    
    for x in [ "osx" , "linux" , "win32" , "sunos5" , "src" ]:
        check_dir( bucket , x )
    

if __name__ == "__main__":
    run()
