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

        inline string _end() { return "</body></html>"; }
        inline string _table() { return "</table>"; }
        inline string _tr() { return "</tr>\n"; }

        inline string tr() { return "<tr>"; }

        inline string td(double x) { 
            stringstream ss;
            ss << "<td>" << x << "</td>";
            return ss.str();
        }
        inline string td(string x) { 
            return "<td>" + x + "</td>";
        }

        inline string table(const char *headers[] = 0) { 
            stringstream ss;
            ss << "<table border=1 cellpadding=2 cellspacing=0>\n";
            if( headers ) { 
                ss << "<tr>";
                while( *headers ) { 
                    ss << "<th>" << *headers << "</th>";
                    headers++;
                }
                ss << "</tr>\n";
            }
            return ss.str();
        }

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
