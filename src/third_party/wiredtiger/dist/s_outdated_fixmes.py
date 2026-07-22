#!/usr/bin/env python3
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

# Identify all FIXME comments in the codebase associated with a WT ticket and
# then confirm all of these tickets are still open. If a closed ticket is found
# report the error.

import argparse, datetime, json, os, pathlib, re, sys, urllib.request

# Grace period: tickets closed as Done/Fixed on a feature branch need time to
# merge back to the develop branch before their FIXMEs are flagged as outdated.
# Any other resolution type, such as Duplicate or Won't Fix, is flagged
# immediately.
GRACE_PERIOD_DAYS = 50
GRACE_PERIOD_RESOLUTIONS = {"Done", "Fixed"}


def all_files():
    """
    List all files in the codebase other than those in the .git and build
    directories.
    """

    for p in pathlib.Path("..").rglob("*"):
        if (
            p.is_file()
            and ".git" not in p.parts
            and "CMakeFiles" not in p.parts
        ):
            yield str(p)


def find_fixme_tickets():
    """
    Return all WT tickets that are associated with a FIXME comment, together
    with the file in which the FIXME was found.
    """

    fixme_tickets = set()

    match_re = re.compile(r"FIX.?ME.*?(WT-[0-9]+)")
    for filepath in all_files():
        try:
            with open(filepath, "r", encoding="utf-8", errors="ignore") as file:
                for match in match_re.finditer(file.read()):
                    fixme_tickets.add((match[1], filepath))
        except Exception:
            # There are files like *.png which cannot be read. In this case
            # skip them silently.
            pass
    return fixme_tickets


def query_jira_ticket(ticket, token):
    """Query Jira for a ticket's resolution and resolution date."""

    url = (
        f"https://jira.mongodb.org/rest/api/2/issue/{ticket}"
        f"?fields=resolution,resolutiondate"
    )

    headers = {"Authorization": f"Bearer {token}"}
    request = urllib.request.Request(url, headers=headers)

    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            fields = json.loads(response.read()).get("fields", {})
    except (urllib.error.URLError, json.JSONDecodeError):
        fields = {}

    resolution = fields.get("resolution") or {}
    return resolution.get("name"), fields.get("resolutiondate")


def is_outdated(resolution, resolution_date):
    """Return True if a ticket's FIXME should be flagged as outdated."""

    if resolution is None:
        return False  # Ticket is not resolved.

    if resolution not in GRACE_PERIOD_RESOLUTIONS:
        return True  # Duplicate, Won't Fix, etc. are flagged immediately.

    if resolution_date is None:
        return True  # No resolution date info. Assume it's outdated.

    # All other resolutions are flagged after a grace period.
    date_format = "%Y-%m-%dT%H:%M:%S.%f%z"
    iso_date = datetime.datetime.strptime(resolution_date, date_format)
    days_since = (datetime.datetime.now(datetime.timezone.utc) - iso_date).days

    return days_since >= GRACE_PERIOD_DAYS


def label_ticket(ticket, token):
    """Add the outdated-fixme label to a Jira ticket."""

    url = f"https://jira.mongodb.org/rest/api/2/issue/{ticket}"

    body = json.dumps(
        {"update": {"labels": [{"add": "outdated-fixme"}]}}
    ).encode()

    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
    }

    request = urllib.request.Request(
        url, data=body, method="PUT", headers=headers
    )

    urllib.request.urlopen(request, timeout=10).close()


def parse_args():
    """Return the parsed command line arguments."""

    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--label-outdated",
        action="store_true",
        help="Add a 'outdated-fixme' label to each outdated ticket found.",
    )

    return parser.parse_args()


def get_jira_token():
    """Return the Jira token, or exit if it's unset."""

    token_name = "JIRA_API_TOKEN"
    token = os.environ.get(token_name)

    if not token:
        sys.exit(
            f"This script requires the {token_name} environment variable to be set."
        )

    return token


def main():
    """
    Query JIRA for all tickets with a FIXME in the codebase. If any of these
    tickets are closed, report them all and return an error code.
    """
    args = parse_args()
    token = get_jira_token()

    closed_ticket_found = False
    outdated_tickets = set()

    for ticket, file in sorted(find_fixme_tickets()):
        query_result = query_jira_ticket(ticket, token)
        if is_outdated(*query_result):
            print(f"{ticket} is a closed ticket that has a FIXME comment in {file}.")
            closed_ticket_found = True
            outdated_tickets.add(ticket)

    if args.label_outdated:
        for ticket in outdated_tickets:
            label_ticket(ticket, token)

    exit_code = 1 if closed_ticket_found else 0
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
