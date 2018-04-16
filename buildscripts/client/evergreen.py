"""Methods for working with evergreen."""
import logging
import os
import time

try:
    from urlparse import urlparse
except ImportError:
    from urllib.parse import urlparse  # type: ignore

import requests
import yaml

LOGGER = logging.getLogger(__name__)

DEFAULT_API_SERVER = "https://evergreen.mongodb.com"


def generate_evergreen_project_name(owner, project, branch):
    """Build an evergreen project name based on the project owner, name and branch."""
    return "{owner}-{project}-{branch}".format(owner=owner, project=project, branch=branch)


def read_evg_config():
    """
    Search known locations for the evergreen config file.

    Read the first on that is found and return the results.
    """
    known_locations = [
        "./.evergreen.yml",
        os.path.expanduser("~/.evergreen.yml"),
        os.path.expanduser("~/cli_bin/.evergreen.yml"),
    ]

    for filename in known_locations:
        if os.path.isfile(filename):
            with open(filename, "r") as fstream:
                return yaml.safe_load(fstream)

    return None


def get_evergreen_server():
    """
    Determine the evergreen server based on config files.

    If it cannot be determined from config files, fallback to the default.
    """
    evg_config = read_evg_config()
    evg_config = evg_config if evg_config is not None else {}
    api_server = "{url.scheme}://{url.netloc}".format(
        url=urlparse(evg_config.get("api_server_host", DEFAULT_API_SERVER)))

    return api_server


def get_evergreen_api():
    """Get an instance of the EvergreenApi object with the default api server."""
    return EvergreenApi(get_evergreen_server())


class EvergreenApi(object):
    """Module for interacting with the evergreen api."""

    def __init__(self, api_server):
        """Initialize the object."""
        self.api_server = api_server

    def get_history(self, project, params):
        """Get the test history from Evergreen."""
        url = "{api_server}/rest/v1/projects/{project}/test_history".format(
            api_server=self.api_server, project=project)

        start = time.time()
        response = requests.get(url=url, params=params)
        LOGGER.debug("Request took %fs:", round(time.time() - start, 2))
        response.raise_for_status()

        return response.json()
