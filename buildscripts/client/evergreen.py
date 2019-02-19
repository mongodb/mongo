"""Methods for working with Evergreen API."""
import logging
import os
import time

try:
    from urllib.parse import urlparse
except ImportError:
    from urllib.parse import urlparse  # type: ignore

import requests
import yaml

from buildscripts.resmokelib import utils

LOGGER = logging.getLogger(__name__)

DEFAULT_API_SERVER = "https://evergreen.mongodb.com"

EVERGREEN_FILES = ["./.evergreen.yml", "~/.evergreen.yml", "~/cli_bin/.evergreen.yml"]


def read_evg_config():
    """
    Search known locations for the Evergreen config file.

    Read the first config file that is found and return the results.

    :return: Evergreen config dict or None.
    """
    known_locations = [os.path.expanduser(evg_file) for evg_file in EVERGREEN_FILES]
    for filename in known_locations:
        if os.path.isfile(filename):
            with open(filename, "r") as fstream:
                return yaml.safe_load(fstream)

    return None


def get_evergreen_headers():
    """Return the Evergreen API headers from the config file.

    :return API headers dict.
    """
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

    :return Evergreen API server string.
    """
    evg_config = read_evg_config()
    evg_config = evg_config if evg_config is not None else {}
    api_server = "{url.scheme}://{url.netloc}".format(
        url=urlparse(evg_config.get("api_server_host", DEFAULT_API_SERVER)))

    return api_server


def get_evergreen_api():
    """Get an instance of the EvergreenApi object.

    :return Evergreen API instance.
    """
    return EvergreenApi(get_evergreen_server())


def get_evergreen_apiv2(**kwargs):
    """Get an instance of the EvergreenApiV2 object.

    :param **kwargs: Named arguments passed to the EvergreenApiV2 class init.
    :return Evergreen API V2 instance.
    """
    return EvergreenApiV2(get_evergreen_server(), get_evergreen_headers(), **kwargs)


class EvergreenApi(object):
    """Module for interacting with the Evergreen API."""

    def __init__(self, api_server=DEFAULT_API_SERVER, api_headers=None):
        """Initialize the object.

        :param api_server: Evergreen API server string.
        :param api_headers: Evergreen API headers dict.
        """
        self.api_server = api_server
        self.api_headers = api_headers

    def get_history(self, project, params):
        """Get the test history from Evergreen.

        :param project: Project string for query.
        :param params: Params dict for API call.
        :return: JSON dict response from API call.
        """
        url = "{}/rest/v1/projects/{}/test_history".format(self.api_server, project)

        start = time.time()
        response = requests.get(url=url, params=params)
        LOGGER.debug("Request took %fs:", round(time.time() - start, 2))
        response.raise_for_status()

        return response.json()


def _check_type(obj, instance_type):
    """Raise error if type mismatch.

    :param obj: Object to check.
    :param instance_type: Python type instance.
    """
    if not isinstance(obj, instance_type):
        raise TypeError("Type error mismatch, expected {}".format(instance_type))


def _add_list_param(params, param_name, param_list):
    """Add the params param_name to comma separated string from param_list.

    :param params: Dict of param_name and param_value.
    :param param_name: String name of param. Raise an exception if it already has been added.
    :param param_list: List of param values to be converted to commad separated string.
    """
    if param_list:
        _check_type(param_list, list)
        if param_name in params:
            raise RuntimeError("Cannot add {} as it already exists".format(param_name))
        params[param_name] = ",".join(param_list)


class EvergreenApiV2(EvergreenApi):
    """Module for interacting with the Evergreen V2 API."""

    DEFAULT_GROUP_NUM_DAYS = 1
    DEFAULT_LIMIT = 1000
    DEFAULT_REQUESTERS = ["mainline"]
    DEFAULT_RETRIES = 3
    DEFAULT_SORT = "earliest"

    def __init__(self, api_server=DEFAULT_API_SERVER, api_headers=None,
                 num_retries=DEFAULT_RETRIES):
        """Initialize the object.

        :param api_server: API server string.
        :param api_headers: API headers dict.
        :param num_retries: Number of retries.
        """
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
        if api_headers:
            self.session.headers.update(api_headers)

    def _task_or_test_stats(  # pylint: disable=too-many-arguments,too-many-locals
            self, endpoint_name, project, after_date, before_date, group_num_days=None,
            requesters=None, sort=None, limit=None, tests=None, tasks=None, variants=None,
            distros=None, group_by=None):
        """Get the task_stats or test_stats from Evergreen.

        :param endpoint_name: API endpoint string.
        :param project: API project string name.
        :param after_date: After date string query parameter.
        :param before_date: Before date string query parameter.
        :param group_num_days: Group number days query parameter.
        :param requesters: List of requesters for query parameter.
        :param sort: Sort string query parameter.
        :param limit: Limit query parameter.
        :param tests: List of tests for query parameter (for 'test_stats' only).
        :param tasks: List of tasks for query parameter.
        :param variants: List of variants for query parameter.
        :param distros: List of distros for query parameter.
        :param group_by: Groups by string query parameter.
        :return: List of stats.
        """
        endpoints = ["test_stats", "task_stats"]
        if endpoint_name not in endpoints:
            raise ValueError("Endpoint name must be one of {}".format(endpoints))
        if endpoint_name == "task_stats" and tests:
            raise ValueError("The task_stats endpoint does not support the 'tests' query parameter")
        params = {
            "sort": utils.default_if_none(sort, self.DEFAULT_SORT),
            "limit": utils.default_if_none(limit, self.DEFAULT_LIMIT),
            "before_date": before_date,
            "after_date": after_date,
            "group_num_days": utils.default_if_none(group_num_days, self.DEFAULT_GROUP_NUM_DAYS)
        } # yapf: disable
        _add_list_param(params, "requesters",
                        utils.default_if_none(requesters, self.DEFAULT_REQUESTERS))
        _add_list_param(params, "tests", tests)
        _add_list_param(params, "tasks", tasks)
        _add_list_param(params, "variants", variants)
        _add_list_param(params, "distros", distros)
        if group_by:
            params["group_by"] = group_by
        url = "{}/rest/v2/projects/{}/{}".format(self.api_server, project, endpoint_name)
        return self._paginate(url, params)

    def test_stats(  # pylint: disable=too-many-arguments
            self, project, after_date, before_date, group_num_days=None, requesters=None, sort=None,
            limit=None, tests=None, tasks=None, variants=None, distros=None, group_by=None):
        """Get the test_stats from Evergreen.

        :param project: API project string name.
        :param after_date: After date string query parameter.
        :param before_date: Before date string query parameter.
        :param group_num_days: Group number days query parameter.
        :param requesters: List of requesters for query parameter.
        :param sort: Sort string query parameter.
        :param limit: Limit query parameter.
        :param tests: List of tests for query parameter.
        :param tasks: List of tasks for query parameter.
        :param variants: List of variants for query parameter.
        :param distros: List of distros for query parameter.
        :param group_by: Groups by string query parameter.
        :return: List of test_stats.
        """
        return self._task_or_test_stats(
            "test_stats", project, after_date=after_date, before_date=before_date,
            group_num_days=group_num_days, requesters=requesters, sort=sort, limit=limit,
            tests=tests, tasks=tasks, variants=variants, distros=distros, group_by=group_by)

    def task_stats(  # pylint: disable=too-many-arguments
            self, project, after_date, before_date, group_num_days=None, requesters=None, sort=None,
            limit=None, tasks=None, variants=None, distros=None, group_by=None):
        """Get the task_stats from Evergreen.

        :param project: API project string name.
        :param after_date: After date string query parameter.
        :param before_date: Before date string query parameter.
        :param group_num_days: Group number days query parameter.
        :param requesters: List of requesters for query parameter.
        :param sort: Sort string query parameter.
        :param limit: Limit query parameter.
        :param tasks: List of tasks for query parameter.
        :param variants: List of variants for query parameter.
        :param distros: List of distros for query parameter.
        :param group_by: Groups by string query parameter.
        :return: List of task_stats.
        """
        return self._task_or_test_stats("task_stats", project, after_date=after_date,
                                        before_date=before_date, group_num_days=group_num_days,
                                        requesters=requesters, sort=sort, limit=limit, tasks=tasks,
                                        variants=variants, distros=distros, group_by=group_by)

    def tasks_by_build_id(self, build_id):
        """Get a list of tasks for the given build.

        :param build_id: Evergreen build to query.
        :return: List of tasks.
        """
        url = "{}/rest/v2/builds/{}/tasks".format(self.api_server, build_id)
        return self._paginate(url)

    def tests_by_task(self, task_id, execution, limit=100, status=None):
        """Return list of tests from the given task_id.

        :param task_id: API Task ID string name.
        :param execution: Execution number query parameter.
        :param limit: Limit query parameter.
        :param status: Status string query parameter to filter on.
        :return: List of tests.
        """
        params = {"execution": execution, "limit": limit}
        if status:
            params["status"] = status
        url = "{}/rest/v2/tasks/{}/tests".format(self.api_server, task_id)
        return self._paginate(url, params=params)

    def project_patches_gen(self, project, limit=100):
        """Return generator for the project patches from Evergreen.

        :param project: API project string name.
        :param limit: Limit query parameter.
        :return: Generator of project patches.
        """
        params = {"limit": limit}
        url = "{}/rest/v2/projects/{}/patches".format(self.api_server, project)
        return self._paginate_gen(url, params)

    def version_builds(self, version):
        """Return list of builds for the specified version from Evergreen.

        :param version: API version string name.
        :return: List of version builds.
        """
        url = "{}/rest/v2/versions/{}/builds".format(self.api_server, version)
        return self._paginate(url)

    def _call_api(self, url, params=None):
        """Return requests.response object or None.

        :param url: URL to retrieve.
        :param params: Dict for query parameters.
        :return: requests.response object.
        """
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
            LOGGER.error("Response text (%s): %s", response.request.url, response.text)
            raise err
        return response

    def _paginate(self, url, params=None):
        """Paginate until all pages are requested and return a list of all JSON results.

        :param url: URL to retrieve.
        :param params: Dict for query parameters.
        :return: Dict of all JSON data.
        """
        return list(self._paginate_gen(url, params))

    def _paginate_gen(self, url, params=None):
        """Return generator of items for each paginated page.

        :param url: URL to retrieve.
        :param params: Dict for query parameters.
        :return: Generator for JSON data.
        """
        while True:
            response = self._call_api(url, params)
            if not response:
                break
            json_response = response.json()
            if json_response:
                if isinstance(json_response, list):
                    for result in json_response:
                        yield result
                else:
                    yield json_response
            url = self._get_next_url(response)
            if not url:
                break
            params = None

    @staticmethod
    def _get_next_url(response):
        return response.links.get("next", {}).get("url")
