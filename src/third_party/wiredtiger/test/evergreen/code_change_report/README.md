# Code Change Report

## Introduction

The WiredTiger Code Change Report provides information about a code change to developers and 
pull request reviewers to help them better understand the level of testing of actual or proposed changes 
and therefore a better understanding of the risk of those changes.
That information can then be factored into their decision-making.

The Code Change Report expands on the information, such as added/changed/removed lines of code,
available in a Git pull request. 
The report adds information from the branch code coverage of tests performed on the changed code
to indicate the branch coverage of the new or changed lines of code.

## Phase 1: Gathering code change info

Phase 1 of the process involves:
* In Evergreen task 'code-coverage' 
  * Run a series of WiredTiger tests with code coverage
  * Use gcovr to combine the per-file code coverage results into a .json file (called `full_coverage_report.json`)
    containing the full code coverage results
* In Evergreen task 'code-change-report':
  * Obtain the diff for the code change:
    * For a patch build, the code change diff is obtained using `git diff --cached > coverage_report/diff.txt` 
      to store the difference in the file `coverage_report/diff.txt`.
    * For a post-merge build, the code change diff is the diff from the most recent commit in the git repository
  * The Python program `code_change_info.py`:
    * Obtains the code change diff
    * Reads the code coverage data from `full_coverage_report.json`
    * Loops through the code change diff, git hunk by git hunk, storing the code changes in a new Python dictionary 
      along with the relevant code coverage info read from `full_coverage_report.json`
    * Writes out a new .json file called `code_change_info.json`

The format of the `code_change_info.json` file is:
* A dictionary where:
  * The key is the path of the file (eg `src/conn/conn_compact.c`)
  * The value is an array where each element describes a code change hunk. 
    Each code change hunk is a dictionary with the following key/value pairs:
    * `status`: with value
      * `A` for code additions
      * `M` for code modifications
    * `new_start`: an integer specifying the number of the first line of the change in the new code
    * `new_lines`: an integer specifying the number lines in the change in the new code
    * `old_start`: an integer specifying the number of the first line of the change in the old code
    * `old_lines`: an integer specifying the number lines in the change in the old code
    * `lines:` a list containing a dictionary per line of code in the hunk with the following key/value pairs:
      * `content`: a string containing the line of code
      * `new_lineno`: an integer specifying the line of the code in the new code, or -1 if the line was deleted
      * `old_linedo`: an integer specifying the line of the code in the old code, or -1 if the line was added
      * `count`: the number of times code in this line of code was executed in the code coverage tests
      * `branches`: a list containing a dictionary per line of branch in this line of code. 
        Note, the list will be empty of there is only one branch in the line of code. 
        The list will either be empty or have at least two members. 
        Each item in the list will contain the following key/value pairs:
        * `count`: the number of times code in this branch was executed in the code coverage tests
        * `fallthrough`: from gcovr - ignored by `code_change_info.json`
        * `throw`: from gcovr - ignored by `code_change_info.json`

Note: 
* According to the gcovr documentation, is possible for gcov to report negative code coverage count values 
  (see https://gcovr.com/en/master/guide/gcov_parser.html#negative-hit-counts for example). 
* If any unexpected results are seen in the code change info, view the results in the gcov output 
  to determine if the issue is in gcov amd/or gcovr, or in the code change info processing.

## Phase 2: The report

The Code Change Report displays a list of all changed files, followed by details of changes to files in the src
directory.

The Code Change Details section contains the lines code in the git diff:
* the 'Count' column shows the number of times code in this line were covered by coverage tests. 
  * the value will be 0 for uncovered code
  * the value will be blank for lines with no coverage data (eg blank lines of code, or deleted code)
* The 'Branches' column displays branch information for any lines of code with branch information 
  (ie where there is more than one branch in the line of code):
  * A small triangle: clicking the small triangle will toggle the display of per-branch coverage information
  * Text: _n_ of _m_ means that n out of m branches were covered. Note that uncovered branches are common in macros.
  * A scale: this shows branch coverage graphically. 
    The colour of the scale will be green for 100% branch coverage, red otherwise. 
* The +-= column:
  * **+** means an added line
  * **-** means a deleted line
  * **=** means an unchanged line
  * Note, a changed line shows as a pair of an added line and a deleted line.
* The code column:
  * Deleted lines are displayed in ~~strikethrough~~
  * Lines of code are coloured based on the line and branch coverage:
    * Green means 100% branch coverage
    * Red means no coverage at all
    * Amber means partial branch coverage (ie some branches are covered, but not others)
    * White means there is no code coverage data. This is typically for blank lines, comments, or statements
      that continue from another line.

