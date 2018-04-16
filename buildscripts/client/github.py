"""Functions for working with github."""
import logging
import time

import requests

LOGGER = logging.getLogger(__name__)

DEFAULT_API_SERVER = "https://api.github.com"


class GithubApi(object):
    """Interface with interacting with the github api."""

    def __init__(self, api_server=DEFAULT_API_SERVER):
        """Create a github api object."""
        self.api_server = api_server

    @staticmethod
    def _make_request(url, params):
        """Make a request to github. Log the request, param and request time."""
        LOGGER.debug("making github request: %s, params=%s", url, params)
        start = time.time()
        response = requests.get(url=url, params=params)
        LOGGER.debug("Request took %fs:", round(time.time() - start, 2))
        response.raise_for_status()

        return response

    @staticmethod
    def _parse_link(response):
        """Parse a github 'Link' header into an object with paginated links."""
        link_object = {}

        if not response.headers["Link"]:
            return link_object

        links = response.headers["Link"].split(",")
        for link in links:
            link_parts = link.split(";")
            link_type = link_parts[1].replace("rel=", "").strip(" \"")
            link_address = link_parts[0].strip("<> ")
            link_object[link_type] = link_address

        return link_object

    def get_commits(self, owner, project, params):
        """Get the list of commits from a specified repository from github."""
        url = "{api_server}/repos/{owner}/{project}/commits".format(api_server=self.api_server,
                                                                    owner=owner, project=project)

        LOGGER.debug("get_commits project=%s/%s, params: %s", owner, project, params)
        response = self._make_request(url, params)
        commits = response.json()

        # If there are more pages of responses, read those as well.
        links = self._parse_link(response)
        while "next" in links:
            response = self._make_request(links["next"], None)
            commits += response.json()

            links = self._parse_link(response)

        LOGGER.debug("Commits from github (count=%d): [%s - %s]", len(commits), commits[-1]["sha"],
                     commits[0]["sha"])

        return commits
