"""Module to access a JIRA server."""
from enum import Enum

import jira
from pydantic import BaseSettings


class SecurityLevel(Enum):
    """Security level of SERVER tickets."""

    MONGO_INTERNAL = "Mongo Internal"
    NONE = "None"


class JiraAuth(BaseSettings):
    """OAuth information to connect to Jira."""

    access_token: str
    access_token_secret: str
    consumer_key: str
    key_cert: str

    class Config:
        """Configuration for JiraAuth."""

        env_prefix = "JIRA_AUTH_"


class JiraClient(object):
    """A client for JIRA."""

    def __init__(self, server: str, jira_auth: JiraAuth) -> None:
        """
        Initialize the JiraClient with the server URL and user credentials.

        :param server: Jira Server to connect to.
        :param jira_auth: OAuth connection information.
        """
        opts = {"server": server, "verify": True}
        self._jira = jira.JIRA(options=opts, oauth=jira_auth.dict(), validate=True)

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
