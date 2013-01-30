
import sys
import codecs

import cpplint
import utils


def run_lint( paths, nudgeOn=False ):
    # errors are as of 10/14
    # idea is not to let it any new type of error
    # as we knock one out, we should remove line
    # note: not all of these are things we want, so please check first

    nudge = [] # things we'd like to turn on sson, so don't make worse
    later = [] # things that are unlikely anytime soon, so meh
    never = [] # things we totally disagree with

    never.append( '-build/header_guard' ) # errors found: 345
    nudge.append( '-build/include' ) # errors found: 924
    nudge.append( '-build/include_order' ) # errors found: 511
    nudge.append( '-build/include_what_you_use' ) # errors found: 986
    nudge.append( '-build/namespaces' ) # errors found: 131
    never.append( '-readability/braces' ) # errors found: 880
    later.append( '-readability/casting' ) # errors found: 748
    nudge.append( '-readability/function' ) # errors found: 49
    later.append( '-readability/streams' ) # errors found: 72
    later.append( '-readability/todo' ) # errors found: 309
    nudge.append( '-runtime/arrays' ) # errors found: 5
    later.append( '-runtime/explicit' ) # errors found: 322
    later.append( '-runtime/int' ) # errors found: 1420
    later.append( '-runtime/printf' ) # errors found: 29
    nudge.append( '-runtime/references' ) # errors found: 1338
    nudge.append( '-runtime/rtti' ) # errors found: 36
    nudge.append( '-runtime/sizeof' ) # errors found: 57
    nudge.append( '-runtime/string' ) # errors found: 6
    nudge.append( '-runtime/threadsafe_fn' ) # errors found: 46
    never.append( '-whitespace/blank_line' ) # errors found: 2080
    never.append( '-whitespace/braces' ) # errors found: 962
    later.append( '-whitespace/comma' ) # errors found: 621
    later.append( '-whitespace/comments' ) # errors found: 2189
    later.append( '-whitespace/end_of_line' ) # errors found: 4340
    later.append( '-whitespace/labels' ) # errors found: 58
    later.append( '-whitespace/line_length' ) # errors found: 14500
    later.append( '-whitespace/newline' ) # errors found: 1520
    nudge.append( '-whitespace/operators' ) # errors found: 2297
    never.append( '-whitespace/parens' ) # errors found: 49058
    nudge.append( '-whitespace/semicolon' ) # errors found: 121
    nudge.append( '-whitespace/tab' ) # errors found: 233

    filters = later + never
    if not nudgeOn:
        filters = filters + nudge

        
    sourceFiles = []
    for x in paths:
        utils.getAllSourceFiles( sourceFiles, x )


    args = [ "--filter=" + ",".join( filters ) , "--counting=detailed" ] + sourceFiles
    filenames = cpplint.ParseArguments( args  )

    def _ourIsTestFilename(fn):
        if fn.find( "dbtests" ) >= 0:
            return True
        if fn.endswith( "_test.cpp" ):
            return True
        return False
    
    cpplint._IsTestFilename = _ourIsTestFilename

    # Change stderr to write with replacement characters so we don't die
    # if we try to print something containing non-ASCII characters.
    sys.stderr = codecs.StreamReaderWriter(sys.stderr,
                                           codecs.getreader('utf8'),
                                           codecs.getwriter('utf8'),
                                           'replace')
    
    cpplint._cpplint_state.ResetErrorCounts()
    for filename in filenames:
        cpplint.ProcessFile(filename, cpplint._cpplint_state.verbose_level)
    cpplint._cpplint_state.PrintErrorCounts()
    
    return cpplint._cpplint_state.error_count == 0


if __name__ == "__main__":
    paths = []
    nudge = False
    
    for arg in sys.argv[1:]:
        if arg.startswith( "--" ):
            arg = arg[2:]
            if arg == "nudge":
                nudge = True
                continue
            else:
                print( "unknown arg [%s]" % arg )
                sys.exit(-1)
        paths.append( arg )

    if len(paths) == 0:
        paths.append( "src/mongo/" )

    if not run_lint( paths, nudge ):
        sys.exit(-1)
