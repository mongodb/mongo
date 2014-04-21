// createCPPfromJavaScriptFiles.js

/*   Copyright 2011 10gen Inc.
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

// This JavaScript file is run under Windows Script Host from the Visual Studio build.
// It creates .CPP files from JavaScript files and is intended to duplicate the functionality
// of the jsToH Python function in SConstruct.  By using only standard Windows components
// (Windows Script Host, JScript) we avoid the need for Visual Studio builders to install
// Python, and we don't need to include the generated files in Git because they can be
// recreated as required.

var whitespace = " \t";
function cppEscape( s ) {
    for ( var i = s.length - 1; i >= 0; --i ) {
        if ( whitespace.indexOf( s.charAt( i ) ) === -1 ) {
            s = s.substr( 0, i + 1 );
            break;
        }
    }
    s = s.replace( /\\/g, "\\\\" );
    s = s.replace( /"/g, '\\"' );
    return s;
};

function jsToH( fso, outputFileNameString, inputFileNameStringArray ) {
    var displayString = 'jsToH( "' + outputFileNameString + '", [';
    var i, len = inputFileNameStringArray.length;
    for ( i = 0; i < len; ++i ) {
        displayString += '"' + inputFileNameStringArray[i] + '"';
        if ( i < len - 1 )
            displayString += ', ';
    }
    displayString += '] );'
    WScript.Echo( displayString );
    var h = ['#include "mongo/base/string_data.h"'
         , 'namespace mongo {'
         , 'struct JSFile{ const char* name; const StringData& source; };'
         , 'namespace JSFiles{'
         ];
    for ( i = 0; i < len; ++i ) {
        var filename = inputFileNameStringArray[i];
        var objname = filename.substring( 0, filename.lastIndexOf( '.' ) ).substr( 1 + filename.lastIndexOf('/') );
        var stringname = '_jscode_raw_' + objname;
        h.push( 'const StringData ' + stringname + ' = ' );
        var inputFile = fso.GetFile( filename );
        var inputStream = inputFile.OpenAsTextStream( 1 /* ForReading */, 0 /* TristateFalse == ASCII */ );
        while ( !inputStream.AtEndOfStream )
            h.push( '"' + cppEscape(inputStream.ReadLine()) + '\\n" ' );
        inputStream.Close();
        h.push( ';' );
        h.push( 'extern const JSFile ' + objname + ';' ); //symbols aren't exported w/o this
        h.push( 'const JSFile ' + objname + ' = { "' + filename + '" , ' + stringname + ' };' );
    }
    h.push( "} // namespace JSFiles" );
    h.push( "} // namespace mongo" );
    h.push( "" );
    var out = fso.CreateTextFile( outputFileNameString, true /* overwrite */ );
    out.Write( h.join( '\n' ) );
    out.Close();
};

function rebuildIfNeeded( fso, outputFileNameString, inputFileNameStringArray ) {
    var rebuildNeeded = false;
    if ( !fso.FileExists( outputFileNameString ) ) {
        rebuildNeeded = true;
    } else {
        var outputFileDate = fso.GetFile( outputFileNameString ).DateLastModified;
        for ( var i = 0, len = inputFileNameStringArray.length; i < len; ++i ) {
            if ( fso.GetFile( inputFileNameStringArray[i] ).DateLastModified > outputFileDate ) {
                rebuildNeeded = true;
                break;
            }
        }
    }
    if ( rebuildNeeded )
        jsToH( fso, outputFileNameString, inputFileNameStringArray );
};

var shell = new ActiveXObject( "WScript.Shell" );
shell.CurrentDirectory = WScript.Arguments.Unnamed.Item( 0 );

var fso = new ActiveXObject( "Scripting.FileSystemObject" );
rebuildIfNeeded(fso, "shell/mongo.cpp", ["shell/assert.js", "shell/types.js", "shell/utils.js", "shell/utils_sh.js",
                                         "shell/utils_auth.js",
                                         "shell/db.js", "shell/mongo.js", "shell/mr.js",
                                         "shell/query.js", "shell/collection.js",
                                         "shell/upgrade_check.js",
                                         "shell/bulk_api.js"]);
rebuildIfNeeded(fso, "shell/mongo-server.cpp", ["shell/servers.js", "shell/shardingtest.js",
                                                "shell/servers_misc.js", "shell/replsettest.js",
                                                "shell/replsetbridge.js"]);
