
import os
import sys

sys.path.append( "." )
sys.path.append( ".." )
sys.path.append( "../../" )
sys.path.append( "../../../" )

import simples3
import settings
import subprocess

# this pushes all source balls as tgz and zip

def run_git( args ):
    cmd = "git " + args
    cmd = cmd.split( " " )
    x = subprocess.Popen( ( "git " + args ).split( " " ) , stdout=subprocess.PIPE).communicate()
    return x[0]
    
def push_tag( bucket , tag , extension , gzip=False ):
    localName = "mongodb-src-" + tag + "." + extension
    remoteName = "src/" + localName
    if gzip:
        remoteName += ".gz"
    for ( key , modify , etag , size ) in bucket.listdir( prefix=remoteName ):
        print( "found old: " + key + " uploaded on: " + str( modify ) )
        return
    
    if os.path.exists( localName ):
        os.remove( localName )

    print( "need to do: " + remoteName )
    
    cmd = "archive --format %s --output %s --prefix mongodb-src-%s/ %s" % ( extension , localName , tag , tag )
    run_git( cmd )

    print( "\t" + cmd )

    if not os.path.exists( localName ) or os.path.getsize(localName) == 0 :
        raise( Exception( "creating archive failed: " + cmd ) )

    if gzip:
        newLocalName = localName + ".gz"
        if ( os.path.exists( newLocalName ) ):
            os.remove( newLocalName )
        subprocess.call( [ "gzip" , localName ] )
        localName = newLocalName

    if not os.path.exists( localName ) or os.path.getsize(localName) == 0 :
        raise( Exception( "gzipping failed" ) )

    bucket.put( remoteName , open( localName , "rb" ).read() , acl="public-read" )
    print( "\t uploaded to: http://s3.amazonaws.com/%s/%s" % ( bucket.name , remoteName ) )
    
    os.remove( localName )


def push_all():
    tags = run_git("tag -l").strip().split( "\n" )

    bucket = simples3.S3Bucket( settings.bucket , settings.id , settings.key )

    for tag in tags:
        push_tag( bucket , tag , "tar" , True )
        push_tag( bucket , tag , "zip" )

if __name__ == "__main__":
    push_all()
