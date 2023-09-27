import argparse
import json
import html


def read_code_change_info(code_change_info_path: str):
    with open(code_change_info_path) as json_file:
        info = json.load(json_file)
        return info


def get_branch_info(branches: list):
    branch_info = list()
    for branch in branches:
        count = branch['count']
        if count < 0:
            count = 0
        branch_info.append(count)
    return branch_info


def get_non_zero_count(value_list: list):
    non_zero_count = 0
    for value in value_list:
        if value != 0:
            non_zero_count += 1
    return non_zero_count


def get_html_colour(count: int, of: int):
    colour = ""
    if count == 0:
        colour = "Red"
    elif count == of:
        colour = "LightGreen"
    else:
        colour = "Orange"

    return colour


def generate_file_info_as_html_text(file: str, file_info: dict, verbose: bool):
    report = list()

    if file.startswith("src/"):
        report.append("<h3>File: {}</h3>\n".format(html.escape(file, quote=True)))

        for hunk in file_info:
            lines = hunk['lines']
            report.append("<table>\n")
            report.append("  <tr>\n")
            report.append("    <th>Count</th>\n")
            report.append("    <th>Branches</th>\n")
            report.append("    <th>=+-</th>\n")
            report.append("    <th></th>\n")
            report.append("  </tr>\n")
            for line in lines:
                new_lineno = line['new_lineno']
                old_lineno = line['old_lineno']
                content = line['content']
                code_colour = "White"
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
                    if count >= 0 and content != '\n':
                        count_str = str(count)
                        code_colour = get_html_colour(count, count)
                report.append("  <tr>\n")
                report.append("    <td>{}</td>\n".format(count_str))
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
                report.append("    <td>{}</td>\n".format(plus_minus))
                report.append("    <td>\n")
                if strikethrough:
                    report.append("    <del>\n")
                report.append("      <p style=\"background-color:{};font-family:\'Courier New\',sans-serif;"
                              "white-space:pre\">{}</p>\n".format(
                        code_colour, html.escape(line['content'], quote=True)))
                if strikethrough:
                    report.append("    </del>\n")
                report.append("    <td>\n")
                report.append("  </tr>\n")
            report.append("</table>\n")

    return report


def generate_html_report_as_text(code_change_info: dict, verbose: bool):
    report = list()

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
    report.append("</style>\n")
    report.append("</head>\n")

    report.append("<body>\n")
    report.append("<h1>Code Change Report</h1>\n")

    # Create table with a list of changed files
    report.append("<table>\n")
    report.append("  <tr>\n")
    report.append("    <th>Changed File(s)</th>\n")
    report.append("  </tr>\n")
    for file in code_change_info:
        report.append("  <tr>\n")
        report.append("    <td>{}</td>\n".format(html.escape(file, quote=True)))
        report.append("  </tr>\n")
    report.append("</table>\n")

    report.append("<p><p>")

    report.append("<h2>Code Change Details</h2>\n")
    report.append("Only files in the 'src' directory are shown below<p>\n")

    # Create per-file info
    for file in code_change_info:
        file_info = code_change_info[file]
        html_lines = generate_file_info_as_html_text(file=file, file_info=file_info, verbose=verbose)
        for line in html_lines:
            report.append(line)

    report.append("</body>\n")
    report.append("</html>\n")

    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--code_change_info', required=True, help='Path to the code change info file')
    parser.add_argument('-o', '--html_output', required=True, help='Path of the html file to write output to')
    parser.add_argument('-v', '--verbose', action="store_true", help='be verbose')
    args = parser.parse_args()

    verbose = args.verbose

    if verbose:
        print('Code Coverage Report')
        print('====================')
        print('Configuration:')
        print('  Code change info file:  {}'.format(args.code_change_info))
        print('  Html output file:  {}'.format(args.html_output))

    code_change_info = read_code_change_info(code_change_info_path=args.code_change_info)
    html_report_as_text = generate_html_report_as_text(code_change_info=code_change_info, verbose=verbose)

    with open(args.html_output, "w") as output_file:
        output_file.writelines(html_report_as_text)


if __name__ == '__main__':
    main()
