#!/usr/bin/env python3

# Identify all FIXME comments in the codebase associated with a WT ticket and then confirm all of
# these tickets are still open. If a closed ticket is found report the error.

import glob, os, re, subprocess, sys

def all_files():
    """
    List all files in the codebase other than those in the .git and build directories.
    """

    excluded_dirs = {"../.git"}

    # The build folder can be identified by the presence of CMakeFiles
    build_file = glob.glob('../**/CMakeFiles')
    if len(build_file) != 0:
        excluded_dirs.add(os.path.dirname(build_file[0]))

    search_function = 'find .. -type f '
    for excluded_dir in excluded_dirs:
        search_function += f'-not -path "{excluded_dir}/*" '

    result = subprocess.run(search_function, shell=True, capture_output=True, text=True)
    return result.stdout.split('\n')

def find_fixme_tickets():
    """
    Return all WT tickets that are associated with a FIXME comment in the codebase.
    """

    fixme_tickets = set()

    match_re = re.compile('FIX.?ME.*?(WT-[0-9]+)')
    for filepath in all_files():
        try:
            with open(filepath, 'r') as file:
                for match in match_re.finditer(file.read()):
                    fixme_tickets.add(match[1])
        except Exception as e:
            # There are files like *.png which cannot be read. In this case skip them silently.
            pass
    return fixme_tickets

def main():
    """
    Query JIRA for all tickets with a FIXME in the codebase. If any of these tickets are closed
    report them all and return an error code.
    """
    closed_ticket_found=False
    STATUS_CATEGORY_DONE='rest/api/2/statuscategory/3'
    for ticket in sorted(find_fixme_tickets()):
        rest_query = f"curl -X GET https://jira.mongodb.org/rest/api/2/issue/{ticket}?fields=status"
        query_result = subprocess.run(rest_query, shell=True, capture_output=True, text=True).stdout
        if STATUS_CATEGORY_DONE in query_result:
            print(f"{ticket} has a FIXME comment in the codebase but the ticket is closed.")
            closed_ticket_found=True

    if closed_ticket_found:
        sys.exit(1)

if __name__ == '__main__':
    main()
