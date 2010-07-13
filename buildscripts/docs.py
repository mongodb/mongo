
import os
import markdown

def convertDir( source , dest ):
    
    if not os.path.exists( dest ):
        os.mkdir( dest )

    for x in os.listdir( source + "/" ):
        if not x.endswith( ".md" ):
            continue

        f = open( source + "/" + x , 'r' )
        raw = f.read()
        f.close()

        html = markdown.markdown( raw )
        print( x )
        
        o = open( dest + "/" + x.replace( ".md" , ".html" ) , 'w' )
        o.write( html )
        o.close()
        


def convertMain():
    convertDir( "docs" , "docs/html" )

if __name__ == "__main__":
    convertMain()

    
