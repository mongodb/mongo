#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

import argparse
import json
import logging
import html
import requests
import textwrap
from code_change_helpers import is_useful_line


# read_code_change_info reads code change info (written by code_change_info.py) from a json file
def read_code_change_info(code_change_info_path: str) -> dict:
    with open(code_change_info_path) as json_file:
        info = json.load(json_file)
        return info


# get_branch_info returns a list of branch counts for a list of branches.
# Negative counts (which gcov can incorrectly return) are corrected to zero.
def get_branch_info(branches: list):
    branch_info = list()
    for branch in branches:
        count = branch['count']
        if count < 0:
            count = 0
        branch_info.append(count)
    return branch_info


# get_non_zero_count returns a count of non-zero values in a list
def get_non_zero_count(value_list: list):
    non_zero_count = 0
    for value in value_list:
        if value != 0:
            non_zero_count += 1
    return non_zero_count


# get_html_colour converts a count and a total into a colour code for highlighting
def get_html_colour(count: int, total: int):
    colour = ""
    if count == 0:
        colour = "LightPink"
    elif count == total:
        colour = "PaleGreen"
    else:
        colour = "SandyBrown"

    return colour


# get_complexity_html_colour converts a complexity value into an html colour string for highlighting.
def get_complexity_html_colour(complexity: int):
    colour = ""
    if complexity <= 10:
        colour = "PaleGreen"
    elif complexity <= 20:
        colour = "Orange"
    elif complexity <= 50:
        colour = "LightPink"
    else:
        colour = "#eb98e8"  # Light purple

    return colour


# get_coverage_html_colour converts a code coverage value into an html colour string for highlighting.
def get_coverage_html_colour(coverage_percent: int):
    colour = ""
    if coverage_percent >= 80:
        colour = "PaleGreen"
    elif coverage_percent >= 60:
        colour = "Orange"
    else:
        colour = "LightPink"

    return colour


# centred_text centres html text
def centred_text(text):
    return "<p style=\"text-align: center\">{}</p>\n".format(text)


# right_text right aligns html text
def right_text(text):
    return "<p style=\"text-align: right\">{}</p>\n".format(text)


def line_number_to_text(code_colour, line_number):
    if line_number > 0:
        return "    <p style=\"background-color:{};text-align: right\">{}</p>\n".format(code_colour, line_number)
    else:
        return ""


def value_as_centred_text(code_colour, value):
    return "    <p style=\"background-color:{};text-align: center\">{}</p>\n".format(code_colour, value)


# generate_summary_table generates the summary table as html
def generate_summary_table(code_change_info: dict) -> list:
    summary_info = code_change_info['summary_info']
    num_lines = int(summary_info['num_lines'])
    num_lines_covered = int(summary_info['num_lines_covered'])
    num_branches = int(summary_info['num_branches'])
    num_branches_covered = int(summary_info['num_branches_covered'])

    branch_coverage_string = ""
    line_coverage_string = ""

    if num_branches > 0:
        branch_coverage_string = coverage_string(num_branches_covered, num_branches)

    if num_lines > 0:
        line_coverage_string = coverage_string(num_lines_covered, num_lines)

    summary_table = []

    summary_table.append("<table class=\"center\">\n")
    summary_table.append("  <tr>\n")
    summary_table.append("    <th>Summary Metric for<br>Added or Changed Code</th>\n")
    summary_table.append("    <th>Value</th>\n")
    summary_table.append("  </tr>\n")
    summary_table.append("    <tr><td>Branch coverage</td><td>{}</td></tr>\n".format(branch_coverage_string))
    summary_table.append("    <tr><td>Covered branches</td><td>{}</td></tr>\n".format(
        centred_text(summary_info['num_branches_covered'])))
    summary_table.append(
        "    <tr><td>Total branches</td><td>{}</td></tr>\n".format(centred_text(summary_info['num_branches'])))
    summary_table.append("    <tr><td>Line coverage</td><td>{}</td></tr>\n".format(line_coverage_string))
    summary_table.append(
        "    <tr><td>Covered lines</td><td>{}</td></tr>\n".format(centred_text(summary_info['num_lines_covered'])))
    summary_table.append(
        "    <tr><td>Total lines</td><td>{}</td></tr>\n".format(centred_text(summary_info['num_lines'])))
    summary_table.append("</table>\n")

    return summary_table


# generate_file_info_as_html_text generates code change info as html text from a particular file
def generate_file_info_as_html_text(file: str, file_info: dict, verbose: bool) -> list:
    report = list()
    code_unhighlighted = "White"

    if file.startswith("src/"):
        escaped_file = html.escape(file, quote=True)
        report.append("<a id=\"{}\"></a>\n".format(escaped_file))
        report.append("<h3>File: {}</h3>\n".format(escaped_file))

        report.append("<table cellpadding=0 cellspacing=0>\n")
        report.append("  <tr>\n")
        report.append("    <th>&nbspCount&nbsp</th>\n")
        report.append("    <th>&nbspBranches&nbsp</th>\n")
        report.append("    <th>&nbspOld line&nbsp</th>\n")
        report.append("    <th>&nbspNew line&nbsp</th>\n")
        report.append("    <th>&nbsp=+-&nbsp</th>\n")
        report.append("    <th></th>\n")
        report.append("  </tr>\n")

        first_line_for_file = True

        for hunk in file_info:
            if not first_line_for_file:
                seperator = "--------"
                report.append("  <tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td></td><td></td></tr>\n".
                              format(centred_text(seperator), centred_text(seperator),
                                     right_text(seperator), right_text(seperator)))
            first_line_for_file = False

            lines = hunk['lines']
            for line in lines:
                new_lineno = line['new_lineno']
                old_lineno = line['old_lineno']
                content = line['content']
                code_colour = code_unhighlighted
                plus_minus = "="
                strikethrough = False
                if new_lineno > 0 > old_lineno:
                    plus_minus = "+"
                if old_lineno > 0 > new_lineno:
                    plus_minus = "-"
                    strikethrough = True
                count_str = ""
                if 'count' in line:
                    count = line['count']
                    if count >= 0 and is_useful_line(content):
                        count_str = str(count)
                        code_colour = get_html_colour(count, count)
                report.append("  <tr>\n")
                report.append("    <td>{}</td>\n".format(right_text(count_str)))
                if 'branches' in line:
                    branches = line['branches']
                    branch_info = get_branch_info(branches=branches)
                    num_branches = len(branches)
                    if num_branches > 0:
                        non_zero_count = get_non_zero_count(branch_info)
                        code_colour = get_html_colour(non_zero_count, num_branches)
                        report.append("    <td>\n")
                        report.append("      <details>\n")
                        report.append("      <summary>\n")
                        report.append("      {} of {}\n".format(non_zero_count, num_branches))
                        #  Use the same value for the low and high values so that anything less than 100% branch
                        #  coverage results in a red scale.
                        meter_high = num_branches * 0.999
                        meter_low = meter_high
                        report.append(
                            "      <meter value=\"{}\" optimum=\"{}\" max=\"{}\" high=\"{}\" low=\"{}\"></meter>".
                            format(non_zero_count, num_branches, num_branches, meter_high, meter_low))
                        report.append("      </summary>\n")
                        report.append("        <div class=\"branchDetails\">\n")
                        for branch_index in range(len(branch_info)):
                            branch_count = branch_info[branch_index]
                            report.append("        <div>")
                            if branch_count > 0:
                                report.append("&check; ")
                            else:
                                report.append("&cross; ")
                            report.append("Branch {} taken {} time(s)".format(branch_index, branch_count))
                            report.append("</div>\n")
                        report.append("      </div>\n")
                        report.append("      </details>\n")
                        report.append("    </td>\n")
                    else:
                        report.append("    <td></td>\n")
                else:
                    report.append("    <td></td>\n")

                report.append("    <td>{}</td>\n".format(line_number_to_text(code_unhighlighted, old_lineno)))
                report.append("    <td>{}</td>\n".format(line_number_to_text(code_colour, new_lineno)))
                report.append("    <td>{}</td>\n".format(centred_text(plus_minus)))
                report.append("    <td>\n")
                if strikethrough:
                    report.append("    <del>\n")
                report.append("      <p style=\"background-color:{};font-family:\'Courier New\',sans-serif;"
                              "white-space:pre\">{}</p>\n".format(
                    code_colour, html.escape(line['content'], quote=True)))
                if strikethrough:
                    report.append("    </del>\n")
                report.append("    <td>\n")

        report.append("</table>\n")

    return report


# change_string generates an html string indicating a change (if any) between two values
def change_string(old_value: int, new_value: int) -> str:
    result = ""
    if new_value > old_value:
        # up arrow
        result = "&#8679;{}".format(new_value - old_value)
    elif new_value < old_value:
        # down arrow
        result = "&#8681;{}".format(old_value - new_value)

    return result


# describe_complexity_categories generates an html table describing the complexity categories
def describe_complexity_categories() -> list:
    code_colour_ = get_complexity_html_colour(1)
    description = list()
    description.append("<table class=\"center\">\n")
    description.append("<tr>\n")
    description.append(
        "<th><a href='https://en.wikipedia.org/wiki/Cyclomatic_complexity'>Cyclomatic complexity</a></th></th>\n")
    description.append(
        "<th><a href='https://en.wikipedia.org/wiki/Cyclomatic_complexity#Interpretation'>Risk evaluation</a></th>\n")
    description.append("</tr>\n")
    description.append("<tr><td> {} </td><td> Simple procedure, little risk </td></tr>\n".
                       format(value_as_centred_text(get_complexity_html_colour(1), "1-10")))
    description.append("<tr><td> {} </td><td> More complex, moderate risk   </td></tr>\n".
                       format(value_as_centred_text(get_complexity_html_colour(11), "11-20")))
    description.append("<tr><td> {} </td><td> Complex, high risk            </td></tr>\n".
                       format(value_as_centred_text(get_complexity_html_colour(21), "21-50")))
    description.append("<tr><td> {} </td><td> Untestable code, very high risk    </td></tr>\n".
                       format(value_as_centred_text(get_complexity_html_colour(51), ">50")))
    description.append("</table>\n")
    return description


# coverage_string converts a coverage value and a total into a colour-coded html string
def coverage_string(covered: int, total: int) -> str:
    string = ""
    if total > 0:
        coverage_percent = int(covered / total * 100)
        colour = get_coverage_html_colour(coverage_percent=coverage_percent)
        string = value_as_centred_text(colour, "{}% ({} of {})".format(coverage_percent, covered, total))
    return string


# Create table with a list of changed functions
def generate_changed_function_table(changed_functions: dict) -> list:
    report = []
    report.append("<table class=\"center\">\n")
    report.append("  <tr>\n")
    report.append("    <th>File</th>\n")
    report.append("    <th>Changed Function(s)</th>\n")
    report.append("    <th>Complexity</th>\n")
    report.append("    <th> </th>")  # a column for the complexity change arrow
    report.append("    <th>Previous<br>Complexity</th>\n")
    report.append("    <th>Branch<br>Coverage</th>\n")
    report.append("    <th>Uncovered<br>Complexity</th>\n")
    report.append("    <th>Lines in<br>function</th>\n")
    report.append("    <th> </th>")  # a column for the lines of code change arrow
    report.append("    <th>Previous lines<br>in function</th>\n")
    report.append("    <th>Line<br>Coverage</th>\n")
    report.append("  </tr>\n")
    for file in changed_functions:
        escaped_file = html.escape(file, quote=True)
        functions_info = changed_functions[file]
        for function in functions_info:
            function_info = functions_info[function]
            complexity = int(function_info['complexity'])
            prev_complexity = -1
            code_colour = get_complexity_html_colour(complexity)
            complexity_string = value_as_centred_text(code_colour, complexity)
            prev_complexity_string = ""
            complexity_change_string = ""
            if 'prev_complexity' in function_info:
                prev_complexity = int(function_info['prev_complexity'])
                code_colour = get_complexity_html_colour(prev_complexity)
                prev_complexity_string = value_as_centred_text(code_colour, prev_complexity)
                complexity_change_string = change_string(old_value=prev_complexity, new_value=complexity)

            num_lines_in_function = int(function_info["num_lines_in_function"])
            num_covered_lines_in_function = int(function_info["num_covered_lines_in_function"])
            num_branches_in_function = int(function_info["num_branches_in_function"])
            num_covered_branches_in_function = int(function_info["num_covered_branches_in_function"])

            uncovered_complexity_string = ""
            if num_branches_in_function > 0:
                uncovered_complexity = complexity * (1 - num_covered_branches_in_function / num_branches_in_function)
                uncovered_complexity_string = "{:.1f}".format(uncovered_complexity)

            branch_coverage_string = coverage_string(num_covered_branches_in_function, num_branches_in_function)
            line_coverage_string = coverage_string(num_covered_lines_in_function, num_lines_in_function)

            lines_in_function = int(function_info['lines_of_code'])
            lines_change_string = ""
            prev_lines_in_function_string = ""
            if 'prev_lines_of_code' in function_info:
                prev_lines_in_function = int(function_info['prev_lines_of_code'])
                lines_change_string = change_string(old_value=prev_lines_in_function, new_value=lines_in_function)
                prev_lines_in_function_string = prev_lines_in_function

            report.append("  <tr>\n")
            report.append("    <td>{}</td>\n".format(escaped_file))
            report.append("    <td>{}</td>\n".format(function_info['name']))
            report.append("    <td>{}</td>\n".format(complexity_string))
            report.append("    <td>{}</td>\n".format(complexity_change_string))
            report.append("    <td>{}</td>\n".format(prev_complexity_string))
            report.append("    <td>{}</td>\n".format(centred_text(branch_coverage_string)))
            report.append("    <td>{}</td>\n".format(centred_text(uncovered_complexity_string)))
            report.append("    <td>{}</td>\n".format(centred_text(lines_in_function)))
            report.append("    <td>{}</td>\n".format(centred_text(lines_change_string)))
            report.append("    <td>{}</td>\n".format(centred_text(prev_lines_in_function_string)))
            report.append("    <td>{}</td>\n".format(centred_text(line_coverage_string)))
            report.append("  </tr>\n")
    report.append("</table>\n")

    report.append("<p>")

    report.extend(describe_complexity_categories())
    return report


# generate_html_report_as_text generates html text from code change info
def generate_html_report_as_text(code_change_info: dict, verbose: bool):
    report = list()
    change_info_list = code_change_info['change_info_list']
    changed_functions = code_change_info['changed_functions']

    report.append("")

    # Create the header and style sections
    report.append("<!DOCTYPE>\n")
    report.append("<html lang=\"\">\n")
    report.append("<head>\n")
    report.append("<title>Code Change Report</title>\n")
    report.append("<style>\n")
    report.append("  .branchDetails\n")
    report.append("  {\n")
    report.append("    font-family: sans-serif;\n")
    report.append("    font-size: small;\n")
    report.append("    text-align: left;\n")
    report.append("    position: absolute;\n")
    report.append("    width: 20em;\n")
    report.append("    padding: 1em;\n")
    report.append("    background: white;\n")
    report.append("    border: solid gray 1px;;\n")
    report.append("    box-shadow: 5px 5px 10px gray;\n")
    report.append("    z-index: 1;\n")  # pop up in front of the main text
    report.append("  }\n")
    report.append("  table\n")
    report.append("  {\n")
    report.append("      border: 1px solid black\n")
    report.append("  }\n")
    report.append("  td\n")
    report.append("  {\n")
    report.append("      padding-left: 5px; padding-right: 5px;\n")
    report.append("  }\n")
    report.append("  table.center\n")
    report.append("  {\n")
    report.append("      margin-left: auto;\n")
    report.append("      margin-right: auto;\n")
    report.append("  }\n")
    report.append("</style>\n")
    report.append("</head>\n")

    report.append("<body>\n")
    report.append("<h1 style=\"text-align: center\">Code Change Report</h1>\n")

    # Create table with a list of changed files
    report.append("<table class=\"center\">\n")
    report.append("  <tr>\n")
    report.append("    <th>Changed File(s), excluding all 0-length or deleted files</th>\n")
    report.append("  </tr>\n")
    for file in change_info_list:
        escaped_file = html.escape(file, quote=True)
        report.append("  <tr><td>\n")
        if file.startswith("src/"):
            report.append("    <a href = \"#{}\">\n".format(escaped_file))
        report.append("      {}\n".format(escaped_file))
        if file.startswith("src/"):
            report.append("    </a>\n")
        report.append("  </td></tr>\n")
    report.append("</table>\n")
    report.append("<p>")

    report.append("<h2 style=\"text-align: center\">Code Change Details</h2>\n")
    report.append(centred_text("Only data on files in the 'src' directory is shown below<p>\n"))

    report.extend(generate_summary_table(code_change_info=code_change_info))
    report.append("<p>")

    report.extend(generate_changed_function_table(changed_functions=changed_functions))

    # Create per-file info
    for file in change_info_list:
        file_info = change_info_list[file]
        html_lines = generate_file_info_as_html_text(file=file, file_info=file_info, verbose=verbose)
        report.extend(html_lines)

    report.append("</body>\n")
    report.append("</html>\n")

    return report


def build_pr_comment(code_change_info: dict, code_change_report_url: str) -> str | None:
    # Do nothing if the PR has no relevant changes.
    if int(code_change_info['summary_info']['num_lines']) == 0:
        logging.info("No need to post a PR comment as there's no relevant change made.")
        return None

    message = ""

    # Code Coverage
    summary_info = code_change_info['summary_info']
    num_lines = int(summary_info['num_lines'])
    num_lines_covered = int(summary_info['num_lines_covered'])
    pct_lines_covered = num_lines_covered / num_lines * 100
    num_branches = int(summary_info['num_branches'])
    num_branches_covered = int(summary_info['num_branches_covered'])

    lines_covered = (
        f"{round(pct_lines_covered)}% ({num_lines_covered}/{num_lines})"
        if num_lines > 0 else
        "N/A"
    )

    branches_covered = "N/A"
    coverage_note = ""

    if num_branches > 0:
        pct_branches_covered = num_branches_covered / num_branches * 100
        branches_covered = f"{round(pct_branches_covered)}% ({num_branches_covered}/{num_branches})"

        if pct_branches_covered >= 80:
            coverage_note = "Woohoo, the code changed in this PR is pretty well tested! :tada:"
        elif pct_branches_covered >= 50 and pct_lines_covered >= 80:
            coverage_note = "Test coverage is ok, please refer to the Code change/coverage report links below and try to improve it if feasible."
        else:
            coverage_note = "Test coverage is very low, please refer to the Code change/coverage report links below and try to improve it if feasible."

    task_name_code_change_report      = "code-change-report"
    task_name_coverage_report_catch2  = "coverage-report-catch2"
    task_name_coverage_report_full    = "generate-coverage-report"
    file_name_code_change_report_html = "code_change_report.html"
    file_name_coverage_report_html    = "1_coverage_report_main.html"
    code_coverage_report_catch2_url   = code_change_report_url.replace(task_name_code_change_report, task_name_coverage_report_catch2).replace(file_name_code_change_report_html, file_name_coverage_report_html)
    code_coverage_report_full_url     = code_change_report_url.replace(task_name_code_change_report, task_name_coverage_report_full).replace(file_name_code_change_report_html, file_name_coverage_report_html)

    message += textwrap.dedent(f"""
        {coverage_note}

        | Metric (for added/changed code) | Coverage |
        |------------------------------------------|-------|
        | Line coverage                            | {lines_covered} |
        | Branch coverage                          | {branches_covered} |

        - [Code change report]({code_change_report_url})
        - [Code coverage report (catch2)]({code_coverage_report_catch2_url})
        - [Code coverage report (full)]({code_coverage_report_full_url})
    """)
    logging.debug(message)

    # Complexity
    changed_functions = code_change_info["changed_functions"]
    threshold_to_warn = 30
    if changed_functions:
        message += "\n\n"

        highest_complexity_touched = max(
            method['complexity']
            for methods in changed_functions.values()
            for method in methods.values()
        )

        if highest_complexity_touched > threshold_to_warn:
            message += ":warning: This PR touches methods that have an extremely high complexity score!\n"

        complexity_warnings = [
            f"In `{filename}` the complexity of `{method}` has increased by {info['complexity'] - info.get('prev_complexity', 0)} to {info['complexity']}."
            for filename, methods in changed_functions.items()
            for method, info in methods.items()
            if info['complexity'] > info.get('prev_complexity', -1) and info['complexity'] > threshold_to_warn
        ]
        for warning in complexity_warnings:
            message += f"- {warning}\n"

    return message


def post_pr_comment(fq_repo, pr_id, token, body):
    url = f"https://api.github.com/repos/{fq_repo}/issues/{pr_id}/comments"
    magic_string = "<!-- STICKY_COMMENT:CODE_QUALITY -->"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3+json"
    }

    # First lets see if an existing comment exists
    # We can cheat here and assume it will be one of the earlier comments, so no need to paginate
    existing = None
    resp = requests.get(url, headers=headers, params={"sort": "created_at", "direction": "asc"})
    resp.raise_for_status()
    for comment in resp.json():
        if magic_string in comment["body"]:
            existing = comment
            break

    if body is None:
        if existing:
            resp = requests.delete(existing["url"], headers=headers)
            resp.raise_for_status()
        return

    # Now create/update the comment with the new contents
    data = {
        "body": f"{body}\n\n{magic_string}"
    }

    if existing:
        resp = requests.patch(existing["url"], json=data, headers=headers)
    else:
        resp = requests.post(url, json=data, headers=headers)
    resp.raise_for_status()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--code_change_info', required=True, help='Path to the code change info file')
    parser.add_argument('-o', '--html_output', required=True, help='Path of the html file to write output to')
    parser.add_argument('--code_change_report_url', help='URL of the code change report')
    parser.add_argument('--github_repo', default='wiredtiger/wiredtiger', help='The github repo for this change')
    parser.add_argument('--github_pr_number', help='A github PR id for this change')
    parser.add_argument('--github_token', help='An API token for github for leaving pr comments')
    parser.add_argument('-v', '--verbose', action="store_true", help='be verbose')
    args = parser.parse_args()

    verbose = args.verbose
    logging.basicConfig(level=logging.DEBUG if verbose else logging.INFO)
    leave_pr_comment = bool(args.github_pr_number and args.github_token)

    if verbose:
        print('Code Coverage Report')
        print('====================')
        print('Configuration:')
        print('  Code change info file:  {}'.format(args.code_change_info))
        print('  Code change report URL:  {}'.format(args.code_change_report_url))
        print('  Html output file:  {}'.format(args.html_output))
        print('  Leave PR Comment: {}'.format(leave_pr_comment))

    code_change_info = read_code_change_info(code_change_info_path=args.code_change_info)
    html_report_as_text = generate_html_report_as_text(code_change_info=code_change_info, verbose=verbose)

    with open(args.html_output, "w") as output_file:
        output_file.writelines(html_report_as_text)

    if leave_pr_comment:
        comment = build_pr_comment(code_change_info=code_change_info, code_change_report_url=args.code_change_report_url)
        post_pr_comment(args.github_repo, args.github_pr_number, args.github_token, comment)


if __name__ == '__main__':
    main()
