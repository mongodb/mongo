// @file html.h

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

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
        inline string _table() { return "</table>\n\n"; }
        inline string _tr() { return "</tr>\n"; }

        inline string tr() { return "<tr>"; }
        inline string tr(const std::string& a, const std::string& b) {
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
        inline string td(const std::string& x) {
            return "<td>" + x + "</td>";
        }
        inline string th(const std::string& x) {
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

        inline string start(const std::string& title) {
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

        inline string red(const std::string& contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#A00;\">" << contentHtml << "</span>";
            return ss.str();
        }
        inline string grey(const std::string& contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#888;\">" << contentHtml << "</span>";
            return ss.str();
        }
        inline string blue(const std::string& contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#00A;\">" << contentHtml << "</span>";
            return ss.str();
        }
        inline string yellow(const std::string& contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#A80;\">" << contentHtml << "</span>";
            return ss.str();
        }
        inline string green(const std::string& contentHtml, bool color=true) {
            if( !color ) return contentHtml;
            stringstream ss;
            ss << "<span style=\"color:#0A0;\">" << contentHtml << "</span>";
            return ss.str();
        }

        inline string p(const std::string& contentHtml) {
            stringstream ss;
            ss << "<p>" << contentHtml << "</p>\n";
            return ss.str();
        }

        inline string h2(const std::string& contentHtml) {
            stringstream ss;
            ss << "<h2>" << contentHtml << "</h2>\n";
            return ss.str();
        }

        /* does NOT escape the strings. */
        inline string a(const std::string& href,
                        const std::string& title="",
                        const std::string& contentHtml = "") {
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

        /* escape for HTML display */
        inline string escape(const string& data) {
            string buffer;
            buffer.reserve( data.size() );
            for( size_t pos = 0; pos != data.size(); ++pos ) {
                switch( data[pos] ) {
                    case '&':
                        buffer.append( "&amp;" );
                        break;
                    case '\"':
                        buffer.append( "&quot;" );
                        break;
                    case '\'':
                        buffer.append( "&apos;" );
                        break;
                    case '<':
                        buffer.append( "&lt;" );
                        break;
                    case '>':
                        buffer.append( "&gt;" );
                        break;
                    default:
                        buffer.append( 1, data[pos] );
                        break;
                }
            }
            return buffer;
        }

    }

}
