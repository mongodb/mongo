"""Module to access a JIRA server."""

import logging
from enum import Enum
from typing import Any, Dict, Iterable, Optional, Sequence

from jira import JIRA, Issue
from jira.client import ResultList
from pydantic import BaseSettings

ASSIGNED_TEAMS_FIELD = "customfield_12751"

logger = logging.getLogger(__name__)


class SecurityLevel(Enum):
    """Security level of SERVER tickets."""

    MONGO_INTERNAL = "Mongo Internal"
    NONE = "None"


class JiraAuth(BaseSettings):
    """Auth information to connect to Jira."""

    access_token: Optional[str]
    access_token_secret: Optional[str]
    consumer_key: Optional[str]
    key_cert: Optional[str]
    pat: Optional[str]

    class Config:
        """Configuration for JiraAuth."""

        env_prefix = "JIRA_AUTH_"

    def get_token_auth(self) -> Optional[str]:
        return self.pat

    def get_oauth(self) -> Optional[Dict[str, Any]]:
        if self.access_token and self.access_token_secret and self.consumer_key and self.key_cert:
            return {
                "access_token": self.access_token,
                "access_token_secret": self.access_token_secret,
                "consumer_key": self.consumer_key,
                "key_cert": self.key_cert,
            }
        return None


class JiraClient:
    """A client for JIRA."""

    def __init__(self, server: str, jira_auth: JiraAuth, dry_run: bool = False) -> None:
        """
        Initialize the JiraClient with the server URL and user credentials.

        :param server: Jira Server to connect to.
        :param jira_auth: Auth connection information.
        """
        opts = {"server": server, "verify": True}
        token_auth = jira_auth.get_token_auth()
        if token_auth:
            self._jira = JIRA(options=opts, validate=True, token_auth=token_auth)
        else:
            self._jira = JIRA(options=opts, validate=True, oauth=jira_auth.get_oauth())
        self.dry_run = dry_run

    def get_ticket_security_level(self, key: str) -> SecurityLevel:
        """
        Lookup the security level of the given ticket.

        :param key: Key of ticket to query.
        :return: Security level of the given ticket.
        """
        ticket = self._jira.issue(key)
        if hasattr(ticket.fields, "security"):
            security_level = ticket.fields.security
            return SecurityLevel(security_level.name)
        return SecurityLevel.NONE

    def get_issues(self, query: str) -> Iterable[Issue]:
        start_at = 0
        max_results = 50
        while True:
            results: ResultList[Issue] = self._jira.search_issues(
                jql_str=query, startAt=start_at, maxResults=max_results
            )
            for item in results:
                yield item

            start_at = results.startAt + results.maxResults
            if start_at > results.total:
                break

    def create_issue(
        self,
        issue_type: str,
        summary: str,
        description: str,
        assigned_teams: Sequence[str],
        jira_project: str,
        owner: Optional[str] = None,
        priority: str = "3",
        components: Optional[Sequence[str]] = None,
        labels: Optional[Sequence[str]] = None,
    ) -> Optional[Issue]:
        assigned_teams_mapped = list(map(lambda x: {"value": x}, assigned_teams))
        fields = {
            "project": {"key": jira_project},
            "summary": summary,
            "description": description,
            "issuetype": {"name": issue_type},
            ASSIGNED_TEAMS_FIELD: assigned_teams_mapped,
            "priority": {"id": priority},
        }
        if labels:
            fields["labels"] = labels

        if owner:
            fields["assignee"] = {"name": owner}

        if components:
            fields["components"] = components

        logger.info({"message": "Creating JIRA issue", "fields": fields})

        if not self.dry_run:
            return self._jira.create_issue(fields=fields)

        return None
