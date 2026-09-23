#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""
Ask Jira whether a ticket has an unresolved linked CVE.

Shipping a security fix before its CVE is published means the commit itself discloses the
vulnerability early. One JQL query answers the question per ticket, and this module is only
that query, so a caller can decide what to do about the answer.

Only the standard library is used, so this imposes no dependencies on its callers. That
includes logging: stdlib logging rather than structlog, so callers that configure structlog
over it see these records in their own format, and callers that do not still get output.
"""

import json
import logging
import os
import urllib.parse
import urllib.request
from typing import Any, Optional

LOGGER = logging.getLogger(__name__)

JIRA_BASE_URL = os.environ.get("JIRA_BASE_URL", "https://jira.mongodb.org")

# Jira project whose issues block a merge. Membership is decided by JQL, not by the key
# prefix; issue type is not considered.
CVE_PROJECT_KEY = "CVE"


def describe(payload: Any) -> str:
    """
    Summarize a decoded response for logging, without dumping its contents.

    Jira payloads run to hundreds of lines, so log the shape, not the body.

    :param payload: Decoded JSON body.
    :return: One-line description.
    """
    if isinstance(payload, list):
        return f"list of {len(payload)}"
    if isinstance(payload, dict):
        return f"object with keys: {', '.join(sorted(payload)[:8])}"
    return type(payload).__name__


def get_json(url: str, headers: dict[str, str], params: Optional[dict[str, str]] = None) -> Any:
    """
    GET a JSON document.

    :param url: Full URL to request.
    :param headers: Request headers, including authorization.
    :param params: Query string parameters.
    :return: Decoded JSON body.
    """
    if params:
        url = f"{url}?{urllib.parse.urlencode(params)}"
    # Safe to log: credentials travel in the Authorization header, never in the URL.
    LOGGER.info("GET %s", url)
    request = urllib.request.Request(url, headers=headers, method="GET")
    with urllib.request.urlopen(request, timeout=30) as response:
        payload = json.loads(response.read().decode("utf-8"))
        LOGGER.info("HTTP %s, %s", response.status, describe(payload))
    return payload


def unresolved_cve_keys(jira_headers: dict[str, str], jira_key: str) -> list[str]:
    """
    Return the unresolved CVE-project issues linked to a ticket, in either direction.

    One query rather than reading the ticket's links and filtering them here. Jira decides
    both what is linked and what is in the CVE project, only keys come back, and the
    project clause always runs -- so an account that cannot see the CVE project gets an
    error rather than an empty answer that looks like a pass.

    :param jira_headers: Authorized Jira headers.
    :param jira_key: Jira issue key, already matched against a key pattern by the caller.
    :return: The linked CVE-project issues that are unresolved.
    """
    results = get_json(
        f"{JIRA_BASE_URL}/rest/api/2/search",
        jira_headers,
        {
            "jql": (
                f'issue in linkedIssues("{jira_key}")'
                f" AND project = {CVE_PROJECT_KEY} AND resolution is EMPTY"
            ),
            "fields": "key",
        },
    )
    unresolved = sorted(issue["key"] for issue in results["issues"])
    LOGGER.info("%s has %s unresolved linked CVE(s)", jira_key, len(unresolved))
    return unresolved


def unresolved_cves_by_ticket(jira_keys: list[str], jira_auth_pat: str) -> dict[str, list[str]]:
    """
    Ask Jira which of the given tickets have an unresolved linked CVE.

    The entry point for callers that only want the answer: it owns the authorization header
    and the one request per ticket that answering takes.

    :param jira_keys: Jira issue keys to check.
    :param jira_auth_pat: Personal access token for the CI Jira account.
    :return: Jira key -> unresolved linked CVE keys, for blocking tickets only.
    """
    jira_headers = {
        "Authorization": f"Bearer {jira_auth_pat}",
        "Accept": "application/json",
    }

    blocking = {}
    for jira_key in jira_keys:
        unresolved = unresolved_cve_keys(jira_headers, jira_key)
        if unresolved:
            blocking[jira_key] = unresolved
    return blocking
