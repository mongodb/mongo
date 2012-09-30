// run_if_newer.js

/*   Copyright 2012 10gen Inc.
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

// This JavaScript file is run under Windows Script Host from the Visual Studio build.
// It compares the timestamps of a set of "input" files with the timestamps of a set of "output"
// files and runs a specified command if any "input" file is newer than any of the "output" files.
//
// Usage:
//  cscript run_if_newer.js /path:"basepath" /input:"filelist" /output:"filelist" /command:"runthis"

function quitWithMessage(message) {
        WScript.Echo("Error in call to \"" + WScript.ScriptName + "\": " + message);
        WScript.Quit(1);
}

function checkArguments(requiredArguments) {
    for (var i = 0, len = requiredArguments.length; i < len; ++i) {
        var argName = requiredArguments[i];
        if (!WScript.Arguments.Named.Exists(argName)) {
            quitWithMessage("missing \"/" + argName + ":\" argument");
        }
        if (WScript.Arguments.Named.Item(argName).length == 0) {
            quitWithMessage("empty \"/" + argName + ":\" argument");
        }
    }
    if (WScript.Arguments.Named.length != requiredArguments.length) {
        quitWithMessage("extra named argument found");
    }
    if (WScript.Arguments.Unnamed.length != 0) {
        quitWithMessage("extra unnamed argument found");
    }
}

function runIfNewer(fso, shell, inputFiles, outputFiles, commandLine) {
    var runCommand = false;
    var outputFileDate = new Date(2037, 11, 31);
    for (var i = 0, len = outputFiles.length; i < len; ++i) {
        var thisFileName = outputFiles[i];
        if (!fso.FileExists(thisFileName)) {
            runCommand = true;
            break;
        }
        var fileDate = fso.GetFile(thisFileName).DateLastModified;
        if (fileDate < outputFileDate) {
            outputFileDate = fileDate;
        }
    }
    for ( var i = 0, len = inputFiles.length; i < len; ++i ) {
        thisFileName = inputFiles[i];
        if (!fso.FileExists(thisFileName)) {
            quitWithMessage("input file \"" + thisFileName + "\" does not exist");
        }
        if (fso.GetFile(thisFileName).DateLastModified >= outputFileDate) {
            runCommand = true;
        }
    }
    if (runCommand) {
        var execObject = shell.Exec(commandLine);
        while (execObject.Status == 0) {
            WScript.Sleep(100);
        }
    }
};

checkArguments(["path", "input", "output", "command"]);

var shell = new ActiveXObject("WScript.Shell");
shell.CurrentDirectory = WScript.Arguments.Named.Item("path");

var fso = new ActiveXObject("Scripting.FileSystemObject");
runIfNewer(fso,
           shell,
           WScript.Arguments.Named.Item("input").split(","),
           WScript.Arguments.Named.Item("output").split(","),
           WScript.Arguments.Named.Item("command"));
