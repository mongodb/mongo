### Description of Debugger docs: how to format, where to install.
### See js/src/doc/README.md for a description.

base-url https://developer.mozilla.org/en-US/docs/Tools/

markdown Debugger-API.md Debugger-API
  label 'debugger'                              "The Debugger API"

markdown Conventions.md Debugger-API/Conventions
  label 'conventions'                           "Debugger API: General Conventions"
  label 'dbg code'      '#debuggee-code'        "Debugger API: General Conventions: Debuggee Code"
  label 'cv'            '#completion-values'    "Debugger API: General Conventions: Completion Values"
  label 'rv'            '#resumption-values'    "Debugger API: General Conventions: Resumption Values"
  label 'wouldrun'      '#the-debugger.debuggeewouldrun-exception' "Debugger API: DebuggeeWouldRun"

markdown Debugger.md Debugger-API/Debugger
  label 'debugger-object'                       "The Debugger object"
  label 'add'           '#addDebuggee'          "The Debugger object: addDebuggee"

markdown Debugger.Environment.md Debugger-API/Debugger.Environment
  label 'environment'                           "Debugger.Environment"

markdown Debugger.Frame.md Debugger-API/Debugger.Frame
  label 'frame'                                 "Debugger.Frame"
  label 'vf'            '#visible-frames'       "Debugger.Frame: Visible Frames"
  label 'generator'     '#generator-frames'     "Debugger.Frame: Generator Frames"
  label 'inv fr'        '#invf'                 "Debugger.Frame: Invocation Frames"
  label 'fr eval'       '#eval'                 "Debugger.Frame: Eval"

markdown Debugger.Object.md Debugger-API/Debugger.Object
  label 'object'                                "Debugger.Object"
  label 'allocation-site' '#allocationsite'     "Debugger.Object: allocationSite"

markdown Debugger.Script.md Debugger-API/Debugger.Script
  label 'script'                                "Debugger.Script"

markdown Debugger.Source.md Debugger-API/Debugger.Source
  label 'source'                                "Debugger.Source"

markdown Debugger.Memory.md Debugger-API/Debugger.Memory
  label 'memory'                                "Debugger.Memory"
  label 'tracking-allocs' '#trackingallocationsites' "Debugger.Memory: trackingAllocationSites"
  label 'drain-alloc-log' '#drain-alloc-log'    "Debugger.Memory: drainAllocationsLog"
  label 'max-alloc-log' '#max-alloc-log'        "Debugger.Memory: maxAllocationsLogLength"
  label 'take-census'   '#take-census'          "Debugger.Memory: takeCensus"

markdown Tutorial-Debugger-Statement.md Debugger-API/Tutorial-Debugger-Statement
  label 'tut debugger'                          "Tutorial: the debugger; statement"

markdown Tutorial-Alloc-Log-Tree.md Debugger-API/Tutorial-Allocation-Log-Tree
  label 'tut alloc log'                         "Tutorial: the allocation log"

# Images:
RBASE=https://mdn.mozillademos.org/files
resource 'img-shadows'            shadows.svg                        $RBASE/7225/shadows.svg
resource 'img-chrome-pref'        enable-chrome-devtools.png         $RBASE/7233/enable-chrome-devtools.png
resource 'img-scratchpad-browser' scratchpad-browser-environment.png $RBASE/7229/scratchpad-browser-environment.png
resource 'img-example-alert'      debugger-alert.png                 $RBASE/7231/debugger-alert.png
resource 'img-alloc-plot'         alloc-plot-console.png             $RBASE/8461/alloc-plot-console.png

# External links:
absolute-label 'protocol' https://wiki.mozilla.org/Remote_Debugging_Protocol "Remote Debugging Protocol"
absolute-label 'saved-frame' https://developer.mozilla.org/en-US/docs/Mozilla/Projects/SpiderMonkey/SavedFrame "SavedFrame"
