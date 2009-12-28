
import os
import sys
import re

def getAllSourceFiles( arr=None , prefix="." ):
    if arr is None:
        arr = []

    for x in os.listdir( prefix ):
        if x.startswith( "." ) or x.startswith( "pcre-" ) or x.startswith( "32bit" ):
            continue
        full = prefix + "/" + x
        if os.path.isdir( full ):
            getAllSourceFiles( arr , full )
        else:
            if full.endswith( ".cpp" ) or full.endswith( ".h" ) or full.endswith( ".c" ):
                arr.append( full )

    return arr
    
assertNames = [ "uassert" , "massert" ]

def assignErrorCodes():
    cur = 10000
    for root in assertNames:
        for x in getAllSourceFiles():
            print( x )
            didAnything = False
            fixed = ""
            for line in open( x ):
                s = line.partition( root + "(" )
                if s[1] == "" or line.startswith( "#define " + root):
                    fixed += line
                    continue
                fixed += s[0] + root + "( " + str( cur ) + " , " + s[2]
                cur = cur + 1
                didAnything = True
            if didAnything:
                out = open( x , 'w' )
                out.write( fixed )
                out.close()


def readErrorCodes( callback ):
    ps = [ re.compile( "([um]asser(t|ted)) *\( *(\d+)" ) ,
           re.compile( "(User|Msg)Exceptio(n)\( *(\d+)" )
           ]
    for x in getAllSourceFiles():
        lineNum = 1
        for line in open( x ):
            for p in ps:               
                for m in p.findall( line ):
                    callback( x , lineNum , line , m[2] )
            lineNum = lineNum + 1
            

def getNextCode():
    highest = [0]
    def check( fileName , lineNum , line , code ):
        code = int( code )
        if code > highest[0]:
            highest[0] = code
    readErrorCodes( check )
    return highest[0] + 1

def checkErrorCodes():
    seen = {}
    errors = []
    def checkDups( fileName , lineNum , line , code ):
        if code in seen:
            print( "DUPLICATE IDS" )
            print( "%s:%d:%s %s" % ( fileName , lineNum , line.strip() , code ) )
            print( "%s:%d:%s %s" % seen[code] )
            errors.append( seen[code] )
        seen[code] = ( fileName , lineNum , line , code )
    readErrorCodes( checkDups )
    return len( errors ) == 0 

if __name__ == "__main__":
    ok = checkErrorCodes()
    print( "ok:" + str( ok ) )
    print( "next: " + str( getNextCode() ) )

