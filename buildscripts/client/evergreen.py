"""Methods for working with Evergreen API."""
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
    """Build an Evergreen project name based on the project owner, name and branch."""
    return "{}-{}-{}".format(owner, project, branch)


def read_evg_config():
    """
    Search known locations for the Evergreen config file.

    Read the first config file that is found and return the results.
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


def get_evergreen_headers():
    """Return the Evergreen API headers from the config file."""
    evg_config = read_evg_config()
    evg_config = evg_config if evg_config is not None else {}
    api_headers = {}
    if evg_config.get("api_key"):
        api_headers["api-key"] = evg_config["api_key"]
    if evg_config.get("user"):
        api_headers["api-user"] = evg_config["user"]
    return api_headers


def get_evergreen_server():
    """
    Determine the Evergreen server based on config files.

    If it cannot be determined from config files, fallback to the default.
    """
    evg_config = read_evg_config()
    evg_config = evg_config if evg_config is not None else {}
    api_server = "{url.scheme}://{url.netloc}".format(
        url=urlparse(evg_config.get("api_server_host", DEFAULT_API_SERVER)))

    return api_server


def get_evergreen_api():
    """Get an instance of the EvergreenApi object."""
    return EvergreenApi(get_evergreen_server())


def get_evergreen_apiv2(**kwargs):
    """Get an instance of the EvergreenApiV2 object."""
    return EvergreenApiV2(get_evergreen_server(), get_evergreen_headers(), **kwargs)


class EvergreenApi(object):
    """Module for interacting with the Evergreen API."""

    def __init__(self, api_server=DEFAULT_API_SERVER, api_headers=None):
        """Initialize the object."""
        self.api_server = api_server
        self.api_headers = api_headers

    def get_history(self, project, params):
        """Get the test history from Evergreen."""
        url = "{}/rest/v1/projects/{}/test_history".format(self.api_server, project)

        start = time.time()
        response = requests.get(url=url, params=params)
        LOGGER.debug("Request took %fs:", round(time.time() - start, 2))
        response.raise_for_status()

        return response.json()


class EvergreenApiV2(EvergreenApi):
    """Module for interacting with the Evergreen V2 API."""

    DEFAULT_LIMIT = 1000
    DEFAULT_REQUESTER = "mainline"
    DEFAULT_RETRIES = 3
    DEFAULT_SORT = "earliest"

    def __init__(self, api_server=DEFAULT_API_SERVER, api_headers=None,
                 num_retries=DEFAULT_RETRIES):
        """Initialize the object."""
        super(EvergreenApiV2, self).__init__(api_server, api_headers)
        self.session = requests.Session()
        retry = requests.packages.urllib3.util.retry.Retry(
            total=num_retries,
            read=num_retries,
            connect=num_retries,
            backoff_factor=0.1,  # Enable backoff starting at 0.1s.
            status_forcelist=[
                500, 502, 504
            ])  # We are not retrying 503 errors as they are used to indicate degraded service
        adapter = requests.adapters.HTTPAdapter(max_retries=retry)
        self.session.mount("{url.scheme}://".format(url=urlparse(api_server)), adapter)
        self.session.headers.update(api_headers)

    def test_stats(  # pylint: disable=too-many-arguments
            self, project, after_date, before_date, group_num_days=1, requester=DEFAULT_REQUESTER,
            sort=DEFAULT_SORT, limit=DEFAULT_LIMIT, tests=None, tasks=None, variants=None,
            distros=None, group_by=None):
        """Get the test_stats from Evergreen."""
        params = {
            "requester": requester, "sort": sort, "limit": limit, "before_date": before_date,
            "after_date": after_date, "group_num_days": group_num_days
        }
        if tests:
            params["tests"] = ",".join(tests)
        if tasks:
            params["tasks"] = ",".join(tasks)
        if variants:
            params["variants"] = ",".join(variants)
        if distros:
            params["distros"] = ",".join(distros)
        if group_by:
            params["group_by"] = group_by
        url = "{}/rest/v2/projects/{}/test_stats".format(self.api_server, project)
        return self._paginate(url, params)

    def tasks_by_build_id(self, build_id):
        """
        Get a list of tasks for the given build.

        :param build_id: Evergreen build to query.
        :return: List of tasks.
        """
        url = "{}/rest/v2/builds/{}/tasks".format(self.api_server, build_id)
        return self._call_api(url).json()

    def _call_api(self, url, params=None):
        start_time = time.time()
        response = self.session.get(url=url, params=params)
        duration = round(time.time() - start_time, 2)
        if duration > 10:
            # If the request took over 10 seconds, increase the log level.
            LOGGER.info("Request %s took %fs:", response.request.url, duration)
        else:
            LOGGER.debug("Request %s took %fs:", response.request.url, duration)
        try:
            response.raise_for_status()
        except requests.exceptions.HTTPError as err:
            LOGGER.error("Response text: %s", response.text)
            raise err
        return response

    def _paginate(self, url, params=None):
        """Paginate until all results are returned and return a list of all JSON results."""
        json_data = []
        while True:
            response = self._call_api(url, params)
            next_page = self._get_next_url(response)
            json_response = response.json()
            if json_response:
                json_data.extend(json_response)
            if not next_page:
                break
            url = next_page
            params = None

        return json_data

    @staticmethod
    def _get_next_url(response):
        return response.links["next"]["url"] if "next" in response.links else None
