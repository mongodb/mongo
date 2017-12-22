import sys

def generate( header, source, language_files ):
    out = open( header, "wb" )
    out.write( """
#pragma once
#include <set>
#include <string>
#include "mongo/util/string_map.h"
namespace mongo {
namespace fts {

  void loadStopWordMap( StringMap< std::set< std::string > >* m );
}
}
""".encode(encoding='utf_8',errors='strict') )
    out.close()



    out = open( source, "wb" )
    tmp_buf = '#include "%s"' % header.rpartition( "/" )[2].rpartition( "\\" )[2] 
    out.write( tmp_buf.encode(encoding='utf_8',errors='strict') )


    out.write( """
namespace mongo {
namespace fts {

  void loadStopWordMap( StringMap< std::set< std::string > >* m ) {
""".encode(encoding='utf_8',errors='strict') )

    for l_file in language_files:
        l = l_file.rpartition( "_" )[2].partition( "." )[0]

        tmp_buf = """  // %s
  {
   const char* const words[] = {
""" % l_file
        out.write( tmp_buf.encode(encoding='utf_8',errors='strict') )
        for word in open( l_file, "rb" ):
            out.write( '       "%s",\n'.encode(encoding='utf_8',errors='strict') % word.strip() )
        tmp_buf="""
   };
   const size_t wordcnt = sizeof(words) / sizeof(words[0]);
   std::set< std::string >& l = (*m)["%s"];
   l.insert(&words[0], &words[wordcnt]);
  }

"""  % l
        out.write( tmp_buf.encode(encoding='utf_8',errors='strict') )
    out.write( """
  }
} // namespace fts
} // namespace mongo
""".encode(encoding='utf_8',errors='strict') )


if __name__ == "__main__":
    generate( sys.argv[ len(sys.argv) - 2],
              sys.argv[ len(sys.argv) - 1],
              sys.argv[1:-2] )
