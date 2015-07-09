Tutorial: Evaluate an Expression When a debugger; Statement Is Executed
=======================================================================

This page shows how you can try out the [`Debugger` API][debugger] yourself
using Firefox's Scratchpad. We use the API to evaluate an expression in the web
page whenever it executes a JavaScript `debugger;` statement.

1)  Visit the URL `about:config`, and set the `devtools.chrome.enabled`
    preference to `true`:

    ![Setting the 'devtools.chrome.enabled' preference][img-chrome-pref]

2)  Save the following HTML text to a file, and visit the file in your
    browser:

    ```language-html
    <div onclick="var x = 'snoo'; debugger;">Click me!</div>
    ```

3)  Open a developer Scratchpad (Menu button > Developer > Scratchpad), and
    select "Browser" from the "Environment" menu. (This menu will not be
    present unless you have changed the preference as explained above.)

    ![Selecting the 'browser' context in the Scratchpad][img-scratchpad-browser]

4)  Enter the following code in the Scratchpad:

    ```language-js
    // This simply defines 'Debugger' in this Scratchpad;
    // it doesn't actually start debugging anything.
    Components.utils.import("resource://gre/modules/jsdebugger.jsm");
    addDebuggerToGlobal(window);

    // Create a 'Debugger' instance.
    var dbg = new Debugger;

    // Get the current tab's content window, and make it a debuggee.
    var w = gBrowser.selectedBrowser.contentWindow.wrappedJSObject;
    dbg.addDebuggee(w);

    // When the debuggee executes a 'debugger' statement, evaluate
    // the expression 'x' in that stack frame, and show its value.
    dbg.onDebuggerStatement = function (frame) {
        alert('hit debugger statement; x = ' + frame.eval('x').return);
    }
    ```

5)  In the Scratchpad, ensure that no text is selected, and press the "Run"
    button.

6)  Now, click on the text that says "Click me!" in the web page. This runs
    the `div` element's `onclick` handler. When control reaches the
    `debugger;` statement, `Debugger` calls your callback function, passing
    a `Debugger.Frame` instance. Your callback function evaluates the
    expression `x` in the given stack frame, and displays the alert:

    ![The Debugger callback displaying an alert][img-example-alert]

7)  Press "Run" in the Scratchpad again. Now, clicking on the "Click me!"
    text causes *two* alerts to show---one for each `Debugger`
    instance.

    Multiple `Debugger` instances can observe the same debuggee. Re-running
    the code in the Scratchpad created a fresh `Debugger` instance, added
    the same web page as its debuggee, and then registered a fresh
    `debugger;` statement handler with the new instance. When you clicked
    on the `div` element, both of them ran. This shows how any number of
    `Debugger`-based tools can observe a single web page
    simultaneously---although, since the order in which their handlers
    run is not specified, such tools should probably only observe, and not
    influence, the debuggee's behavior.

8)  Close the web page and the Scratchpad.

    Since both the Scratchpad's global object and the debuggee window are
    now gone, the `Debugger` instances will be garbage collected, since
    they can no longer have any visible effect on Firefox's behavior. The
    `Debugger` API tries to interact with garbage collection as
    transparently as possible; for example, if both a `Debugger.Object`
    instance and its referent are not reachable, they will both be
    collected, even while the `Debugger` instance to which the shadow
    belonged continues to exist.
