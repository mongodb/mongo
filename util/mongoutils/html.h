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

        inline string _end() { return "</div></body></html>"; }
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
        
        inline void tablecell( stringstream& ss , bool b , bool centered ) {
             tablecell( ss, (b ? "x" : ""), centered);
        }
        
        inline void tablecell( stringstream& ss , bool b ) {
             tablecell( ss, b, false );
        }

        template< typename T>
        inline void tablecell( stringstream& ss , const T& t, bool centered ) {
             ss << "<td";
             if ( centered ) {
                ss << " class=c";
             }
             ss << ">" << t << "</td>";
        }
        
        template< typename T>
        inline void tablecell( stringstream& ss , const T& t ) {
             tablecell( ss, t, false );
        }

        inline string table(const char *headers[] = 0) {
            stringstream ss;
            ss << "\n<table>";
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
            ss << "<html><head>\n<title>mongodb ";
            ss << title;
            ss << "</title>\n";
            ss << "<link href=\"data:image/x-icon;base64,AAABAAEAEBAAAAEAIABoBAAAFgAAACgAAAAQAAAAIAAAAAEAIAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAUITT/FCE0/xQhNP8UITT/FCE0/xQhNP8UITT/FCE0/x8tPv8UITT/FCE0/xQhNP8UITT/FCE0/xQhNP8UITT/FCE0/xQhNP8UITT/FCE0/xQhNP8UITT/FCE0/xYjNf9NX2r/FCE0/xQhNP8UITT/FCE0/xQhNP8UITT/FCE0/xQhNP8UITT/FSI0/xUiNP8VIjT/FSI0/xQhNP8iPzb/UXBp/xUiM/8VIjT/FSI0/xUiNP8VIjT/FSI0/xUiNP8UITT/FCE0/xUiNP8VIjT/FSE0/xUhNP8dPS3/PJkf/z2IMP8kVSb/FSE0/xUhNP8VITT/FSE0/xUhNP8VITT/FCE0/xQhNP8VIjT/FSI0/xUhNP8YKzH/NpIX/0ChI/80iR//MoYc/x9CLP8WIjT/FiI0/xYiNP8WIjT/FSI0/xQhNP8UITT/FSI0/xUiNP8VITT/J2Mh/zudGP9Coin/N4wh/zOHHP8udSD/FiI0/xYiNP8WIjT/FiI0/xUiNP8UITT/FCE0/xUiNP8VIjT/FSIz/zOOFv89nh3/RaQw/zmPJP80iB3/M4cd/xowMf8WIjX/FiI1/xYiNf8VIjT/FCE0/xQhNP8VIjT/FSI0/xgrMP84mxX/QKAi/0imN/86kCf/NIkg/zSJIP8eQC7/FiI1/xYiNf8WIjX/FSI0/xQhNP8UITT/FSI0/xUiNP8YLTD/O5wZ/0KiKf9MqTz/O5En/zWKIP81iyH/H0Ev/xcjNf8XIzX/FyM1/xYiNP8UITT/FCE0/xUiNP8VIjT/FiQz/zyZHv9FpDD/T6tB/zyTKf82jSH/Nowh/xw0Mf8XIzX/FyM1/xcjNf8WIjT/FCE0/xQhNP8VIjT/FSI0/xUhNP82gij/SKY3/1OuRv89lCv/No4j/zKAJf8XIzT/FyM1/xcjNv8XIzb/FiI1/xQhNP8UITT/FSI0/xUiNP8VITT/KFUy/0ypPP9WsEz/PpUt/zePJP8mVS3/FyM1/xcjNf8XIzb/GCQ2/xYiNf8UITT/FCE0/xUiNP8VIjT/FSE0/xckNP9GlD//W7NS/z6WLv8yfSj/FyY0/xYjNf8WIzX/FiM1/xYjNf8WIjT/FCE0/xQhNP8VIjT/FSI0/xUhNP8WITT/ITs4/1qsVv87jS//GzIy/xYiNP8WIjT/FiI0/xYiNP8WIjT/FSI0/xQhNP8UITT/FSI0/xUiNP8VITT/FSE0/xUhM/8mQT3/I0Q1/xUhNP8VITT/FSE0/xUhNP8VITT/FSE0/xUhNP8UITT/FCE0/xUiNP8VIjT/FSI0/xUiNP8VIjT/FSI0/xUiNP8VIjT/FSI0/xUiNP8VIjT/FSI0/xUiNP8VIjT/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==\" rel=\"icon\" type=\"image/x-icon\">\n";
            ss << "<style type=\"text/css\" media=\"screen\">\n"
              "*{padding:0;margin:0;border:none}\n"
              "body{background:#fff;}\n"
              "a{text-decoration:none;color:#1756C3;}\n"
              "a:hover{text-decoration:underline;}\n"
              "body, th, td{font-family:verdana, helvetica, arial, sans-serif;font-size:90%;}\n"
              "table{border-collapse:collapse;}\n"
              "th,td{padding:7px;border-bottom:1px solid #e0e0e0;}\n"
              "th.c, td.c{text-align:center}\n"
              "th{color:#666;text-align:left;}\n"
              "h1{font-size:22px;position:absolute;top:14px;left:160px;}\n"
              "h2{color:#333;margin:40px 0 5px 0;font-size:110%;width:960px;background:#f0f0f0;padding:4px;border:1px solid #e0e0e0;border-radius:4px;position:relative;}\n"
              "h2 .note{color:#666;float:right;margin-right:20px;font-weight:normal;font-style:italic;font-size:80%;line-height:18px;}\n"
              "h2 .note a{position:absolute;top:5px;right:0}\n"
              "pre{padding-left:7px}\n"
              "#page{margin:20px;position:relative}\n"
              "#dbtop .alt{background:#eee;}\n"
              "#logo{width:150px;height:44px;}\n"
              "#menu {position:absolute;top:20px;left:700px;}\n"
              "#commandList a{display:inline-block;padding:7px;}\n"
              "#legend{margin-top:10px;font-color:#666;font-size:90%;padding:7px}\n"
              "#legend span{display:inline-block;padding-right:10px;}\n"
              "#oplog table{font-size:95%}\n"
              "div.info{padding:5px;border-bottom:1px solid #e3e3e3;width:960px;}\n"
              "div.info label{color:#444;float:left;width:175px;}\n"
              "div.info div{margin-left:175px;}\n"
              "div.help{width:16px;height:16px;background:url('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAABHNCSVQICAgIfAhkiAAAAAlwSFlzAAAK8AAACvABQqw0mAAAABh0RVh0U29mdHdhcmUAQWRvYmUgRmlyZXdvcmtzT7MfTgAAAkBJREFUOI2Vk09MknEYx5/3RUiKiBVdgkAQLbsUtXWg2hoYgpvr8GJeKp0Xa6wNo5V2UmvNLpbHDjW7KQcceCRRW7Nu+oLIG39eaExdB6C1tHobv6cT7k1e3Hq25/D77fP9Pd/n93t+gIhQL5+OjJ6ZfPHyHAAo6jEUIsLeSKcz/TRN3yyVS82EEJlard5QNjaGm03miQqSn2K2QbygKIrO57/MJDnOE5gJAM/zQAgBnU6nc7ldF7M5nunv67O/mZr6tqsRO0gmuUGO4ybGxp6AVnsMGIYBuVwO4fAc5PN5GLgzAM5r7ZMWi8W3K6r2AgBUMsnFGcaDVusFDAZnsVwuPysUCt5odOG3zXYZu7quIxuLp6qFERFocQsIeAgRoK3tNFjPW9MajeZxjGU/KQ8qkRACgiAAqVQa9Ho9VeMAEWFxYZFZWVn9Gouvbc3PR22JxPrVVDr9o9tzA1ssrTg+/hzTmcwrsabmWTqdHUecjnZVKBQ6G4+v/XG7O7HJaEK//wHyfC41PDR8fN8DqpnJZD/03u5Fo8GIjx4O4cbmJjs6MnJiLycp9t71apaW3n8/1dKKDrsDc7k839Pdc1SK/WcOqrGzs/1LpVJtmUzmw3aHHQRBeD0dmC5JsXVbYNnY20RiHbM8j8Hg7K16nORmODzXFIm8Eww6PV6xXcJVlv1M07RMiqWlXFEUTQGAnKIokMlkQAjKofbL1I6yOJaXP94jiL4DCsV2sVj0u1wdkf+6A0SEQd99s+GkQbsf8xd2ZfI8FNzshAAAAABJRU5ErkJggg==') no-repeat}\n"
               "</style>\n";

            ss << "</head>\n<body><div id=\"page\"><img id=\"logo\" src=\"data:image/png;base64,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\" />\n";
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

        inline string h1(string contentHtml) {
            stringstream ss;
            ss << "<h1>" << contentHtml << "</h1>\n";
            return ss.str();
        }
        
        inline string h2(string contentHtml) {
            stringstream ss;
            ss << "<h2>" << contentHtml << "</h2>\n";
            return ss.str();
        }
        
        inline string labelValue(string label, string value) {
          stringstream ss;
          ss << "<div class=\"info\"><label>" << label << "</label>" << "<div>" << value << "</div></div>";
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
