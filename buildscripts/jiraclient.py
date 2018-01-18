"""Module to access a JIRA server."""

from __future__ import absolute_import

import jira


class JiraClient(object):
    """A client for JIRA."""
    CLOSE_TRANSITION_NAME = "Close Issue"
    RESOLVE_TRANSITION_NAME = "Resolve Issue"
    FIXED_RESOLUTION_NAME = "Fixed"
    WONT_FIX_RESOLUTION_NAME = "Won't Fix"

    def __init__(self,
                 server,
                 username=None,
                 password=None,
                 access_token=None,
                 access_token_secret=None,
                 consumer_key=None,
                 key_cert=None):
        """Initialize the JiraClient with the server URL and user credentials."""
        opts = {"server": server, "verify": True}
        basic_auth = None
        oauth_dict = None
        if access_token and access_token_secret and consumer_key and key_cert:
            oauth_dict = {
                "access_token": access_token,
                "access_token_secret": access_token_secret,
                "consumer_key": consumer_key,
                "key_cert": key_cert
            }
        elif username and password:
            basic_auth = (username, password)
        else:
            raise TypeError("Must specify Basic Auth (using arguments username & password)"
                            " or OAuth (using arguments access_token, access_token_secret,"
                            " consumer_key & key_cert_file) credentials")
        self._jira = jira.JIRA(
            options=opts, basic_auth=basic_auth, oauth=oauth_dict, validate=True)

        self._transitions = {}
        self._resolutions = {}

    def create_issue(self, project, summary, description, labels=None):
        """Create an issue."""
        fields = {"project": project,
                  "issuetype": {"name": "Task"},
                  "summary": summary,
                  "description": description}
        new_issue = self._jira.create_issue(fields=fields)
        if labels:
            new_issue.update(fields={"labels": labels})
        return new_issue.key

    def close_issue(self, issue_key, resolution, fix_version=None):
        """Close an issue."""
        issue = self._jira.issue(issue_key)
        resolution_id = self._get_resolution_id(resolution)
        if resolution_id is None:
            raise ValueError("No resolution found for '{0}'. Leaving issue '{1}' open.".format(
                resolution, issue_key))
        close_transition_id = self._get_transition_id(issue, JiraClient.CLOSE_TRANSITION_NAME)
        if close_transition_id is None:
            raise ValueError("No transition found for '{0}'. Leaving issue '{1}' open.".format(
                JiraClient.CLOSE_TRANSITION_NAME, issue_key))
        fields = {"resolution": {"id": resolution_id}}
        if fix_version:
            fields["fixVersions"] = [{"name": fix_version}]
        self._jira.transition_issue(issue, close_transition_id, fields=fields)

    def _get_transition_id(self, issue, name):
        project_key = issue.fields.project.key
        project_transitions = self._transitions.setdefault(project_key, {})

        transition_id = project_transitions.get(name)
        if transition_id:
            return transition_id
        transitions = self._jira.transitions(issue)
        for transition in transitions:
            project_transitions[transition["name"]] = transition["id"]
            if transition["name"] == name:
                transition_id = transition["id"]
        return transition_id

    def _get_resolution_id(self, name):
        resolution_id = self._resolutions.get(name)
        if resolution_id is not None:
            return resolution_id
        resolutions = self._jira.resolutions()
        for resolution in resolutions:
            self._resolutions[resolution.name] = resolution.id
            if resolution.name == name:
                resolution_id = resolution.id
        return resolution_id
