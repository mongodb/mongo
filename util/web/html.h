// @file html.h

#pragma once

/* Things in the mongoutils namespace 
   (1) are not database specific, rather, true utilities
   (2) are cross platform
   (3) may require boost headers, but not libs
   (4) are clean and easy to use in any c++ project without pulling in lots of other stuff
*/

#include <sstream>

namespace mongoutils {

    namespace html {

        using namespace std;

        const char *_end = "</body></html>";

        inline string start(string title) { 
            return string("<html><head><title>") + title + "</title></head><body>";
        }

        inline string p(string contentHtml) {
            stringstream ss;
            ss << "<p>" << contentHtml << "</p>\n";
            return ss.str();
        }

        /* does NOT escape the strings. */
        inline string a(string href, string title="", string contentHtml = "") { 
            stringstream ss;
            ss << "<a";
            if( !href.empty() ) ss << " href=\"" << href << '"';
            if( !title.empty() ) ss << " title=\"" << title << '"';
            ss << '>';
            if( !contentHtml.empty() ) {
                ss << contentHtml << "</a>";
            }
            return ss.str();
        }

    }

}
