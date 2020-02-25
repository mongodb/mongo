import sys

def generate( header, source, language_files ):
    out = open( header, "w" )
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
""" )
    out.close()



    out = open( source, "w", encoding='utf-8')
    out.write( '#include "{}"'.format(header.rpartition( "/" )[2].rpartition( "\\" )[2]) )
    out.write( """
namespace mongo {
namespace fts {

  void loadStopWordMap( StringMap< std::set< std::string > >* m ) {

    m->insert({
""" )

    for l_file in language_files:
        l = l_file.rpartition( "_" )[2].partition( "." )[0]

        out.write( '  // %s\n' % l_file )
        out.write( '  {\n' )
        out.write( '    "%s", {\n' % l )
        for word in open( l_file, "rb" ):
            out.write( '       "%s",\n' % word.decode('utf-8').strip() )
        out.write( '  }},\n' )

    out.write( """
    });
  }
} // namespace fts
} // namespace mongo
""" )


if __name__ == "__main__":
    generate( sys.argv[ len(sys.argv) - 2],
              sys.argv[ len(sys.argv) - 1],
              sys.argv[1:-2] )
