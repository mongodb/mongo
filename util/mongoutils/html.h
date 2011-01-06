// @file html.h

#pragma once

/* Things in the mongoutils namespace
   (1) are not database specific, rather, true utilities
   (2) are cross platform
   (3) may require boost headers, but not libs
   (4) are clean and easy to use in any c++ project without pulling in lots of other stuff
*/

/*    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <sstream>

namespace mongoutils {

    namespace html {

        using namespace std;

        inline string _end() { return "</body></html>"; }
        inline string _table() { return "</table>\n\n"; }
        inline string _tr() { return "</tr>\n"; }

        inline string tr() { return "<tr>"; }
        inline string tr(string a, string b) {
            stringstream ss;
            ss << "<tr><td>" << a << "</td><td>" << b << "</td></tr>\n";
            return ss.str();
        }
        template <class T>
        inline string td(T x) {
            stringstream ss;
            ss << "<td>" << x << "</td>";
            return ss.str();
        }
        inline string td(string x) {
            return "<td>" + x + "</td>";
        }
        inline string th(string x) {
            return "<th>" + x + "</th>";
        }

        inline void tablecell( stringstream& ss , bool b ) {
            ss << "<td>" << (b ? "<b>X</b>" : "") << "</td>";
        }

        template< typename T>
        inline void tablecell( stringstream& ss , const T& t ) {
            ss << "<td>" << t << "</td>";
        }

        inline string table(const char *headers[] = 0, bool border = true) {
            stringstream ss;
            ss << "\n<table "
               << (border?"border=1 ":"")
               << "cellpadding=2 cellspacing=0>\n";
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
            stringstream ss;
            ss << "<html><head>\n<title>";
            ss << title;
            ss << "</title>\n";

            ss << "<style type=\"text/css\" media=\"screen\">"
               "body { font-family: helvetica, arial, san-serif }\n"
               "table { border-collapse:collapse; border-color:#999; margin-top:.5em }\n"
               "th { background-color:#bbb; color:#000 }\n"
               "td,th { padding:.25em }\n"
               "</style>\n";

            ss << "</head>\n<body>\n";
            return ss.str();
        }

        inline string red(string contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#A00;\">" << contentHtml << "</span>";
            return ss.str();
        }
        inline string grey(string contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#888;\">" << contentHtml << "</span>";
            return ss.str();
        }
        inline string blue(string contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#00A;\">" << contentHtml << "</span>";
            return ss.str();
        }
        inline string yellow(string contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#A80;\">" << contentHtml << "</span>";
            return ss.str();
        }
        inline string green(string contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#0A0;\">" << contentHtml << "</span>";
            return ss.str();
        }

        inline string p(string contentHtml) {
            stringstream ss;
            ss << "<p>" << contentHtml << "</p>\n";
            return ss.str();
        }

        inline string h2(string contentHtml) {
            stringstream ss;
            ss << "<h2>" << contentHtml << "</h2>\n";
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
