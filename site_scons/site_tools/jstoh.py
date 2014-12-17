import os
import sys

def jsToHeader(target, source):

    outFile = target

    h =  ['#include "mongo/base/string_data.h"'
        ,'namespace mongo {'
        ,'struct JSFile{ const char* name; const StringData& source; };'
        ,'namespace JSFiles{'
         ]

    def cppEscape(s):
        s = s.rstrip()
        s = s.replace( '\\', '\\\\' )
        s = s.replace( '"', r'\"' )
        return s

    for s in source:
        filename = str(s)
        objname = os.path.split(filename)[1].split('.')[0]
        stringname = '_jscode_raw_' + objname

        h.append('const StringData ' + stringname + " = ")

        for l in open( filename, 'r' ):
            h.append( '"' + cppEscape(l) + r'\n" ' )

        h.append(";")
        h.append('extern const JSFile %s;'%objname) #symbols aren't exported w/o this
        h.append('const JSFile %s = { "%s", %s };'%(objname, filename.replace('\\', '/'), stringname))

    h.append("} // namespace JSFiles")
    h.append("} // namespace mongo")
    h.append("")

    text = '\n'.join(h);

    print "writing: %s" % outFile
    out = open( outFile, 'wb' )
    try:
        out.write( text )
    finally:
        out.close()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print "Must specify [target] [source] "
        sys.exit(1);

    jsToHeader(sys.argv[1], sys.argv[2:])


