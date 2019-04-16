"""Unit tests for the client.evergreen module."""

import datetime
import unittest

import requests

from mock import Mock, MagicMock, call, mock_open, patch

import buildscripts.client.evergreen as evergreen

# pylint: disable=missing-docstring,protected-access,attribute-defined-outside-init,too-many-instance-attributes

EVERGREEN = "buildscripts.client.evergreen"


class TestReadEvgConfig(unittest.TestCase):
    def test_read_evg_config(self):
        evg_yaml = "data1: val1\ndata2: val2"
        with patch("os.path.isfile", return_value=True),\
             patch(EVERGREEN + ".open", mock_open(read_data=evg_yaml)):
            evg_config = evergreen.read_evg_config()
            self.assertEqual(evg_config["data1"], "val1")
            self.assertEqual(evg_config["data2"], "val2")

    def test_read_evg_config_file_order(self):
        with patch("os.path.isfile", return_value=False) as mock_isfile,\
             patch("os.path.expanduser", side_effect=(lambda path: path)):
            self.assertIsNone(evergreen.read_evg_config())
            self.assertEqual(mock_isfile.call_count, len(evergreen.EVERGREEN_FILES))
            calls = [call(evg_file) for evg_file in evergreen.EVERGREEN_FILES]
            mock_isfile.assert_has_calls(calls)

    def test_read_evg_config_file_not_found(self):
        with patch("os.path.isfile", return_value=False):
            self.assertIsNone(evergreen.read_evg_config())

    def test_read_evg_config_last_file(self):
        evg_files = {"path1": False, "path2": False, "path3": True}
        evg_paths = ["path1", "path2", "path3"]
        evg_yaml = "data1: val1\ndata2: val2"
        with patch("os.path.isfile", lambda path: evg_files[path]),\
             patch(EVERGREEN + ".open", mock_open(read_data=evg_yaml)) as mock_openfile,\
             patch(EVERGREEN + ".EVERGREEN_FILES", evg_paths):
            evg_config = evergreen.read_evg_config()
            mock_openfile.assert_called_once_with("path3", "r")
            self.assertEqual(evg_config["data1"], "val1")
            self.assertEqual(evg_config["data2"], "val2")


class TestGetEvergreenHeaders(unittest.TestCase):
    def test_get_evergreen_headers(self):
        api_key = "mykey"
        user = "me"
        evg_config = {"api_key": api_key, "user": user}
        with patch(EVERGREEN + ".read_evg_config", return_value=evg_config):
            api_headers = evergreen.get_evergreen_headers()
            self.assertEqual(api_key, api_headers["api-key"])
            self.assertEqual(user, api_headers["api-user"])

    def test_get_evergreen_headers_none(self):
        with patch(EVERGREEN + ".read_evg_config", return_value=None):
            api_headers = evergreen.get_evergreen_headers()
            self.assertDictEqual({}, api_headers)

    def test_get_evergreen_headers_no_data(self):
        evg_config = {}
        with patch(EVERGREEN + ".read_evg_config", return_value=evg_config):
            api_headers = evergreen.get_evergreen_headers()
            self.assertDictEqual(evg_config, api_headers)

    def test_get_evergreen_headers_no_user(self):
        data_key = "data1"
        data_val = "val1"
        api_key = "mykey"
        evg_config = {"api_key": api_key, data_key: data_val}
        with patch(EVERGREEN + ".read_evg_config", return_value=evg_config):
            api_headers = evergreen.get_evergreen_headers()
            self.assertEqual(api_key, api_headers["api-key"])
            self.assertNotIn("user", api_headers)
            self.assertNotIn(data_key, api_headers)

    def test_get_evergreen_headers_no_api_key(self):
        data_key = "data1"
        data_val = "val1"
        user = "me"
        evg_config = {"user": user, data_key: data_val}
        with patch(EVERGREEN + ".read_evg_config", return_value=evg_config):
            api_headers = evergreen.get_evergreen_headers()
            self.assertEqual(user, api_headers["api-user"])
            self.assertNotIn("api-key", api_headers)
            self.assertNotIn(data_key, api_headers)


class TestGetEvergreenServer(unittest.TestCase):
    def test_get_evergreen_server(self):
        api_server_host = "https://myevergreen.com"
        evg_config = {"api_server_host": api_server_host}
        with patch(EVERGREEN + ".read_evg_config", return_value=evg_config):
            api_server = evergreen.get_evergreen_server()
            self.assertEqual(api_server_host, api_server)

    def test_get_evergreen_server_none(self):
        with patch(EVERGREEN + ".read_evg_config", return_value=None):
            api_server = evergreen.get_evergreen_server()
            self.assertEqual(evergreen.DEFAULT_API_SERVER, api_server)

    def test_get_evergreen_server_default(self):
        evg_config = {}
        with patch(EVERGREEN + ".read_evg_config", return_value=evg_config):
            api_server = evergreen.get_evergreen_server()
            self.assertEqual(evergreen.DEFAULT_API_SERVER, api_server)

    def test_get_evergreen_server_other_data(self):
        evg_config = {"api_server_host_bad": "bad"}
        with patch(EVERGREEN + ".read_evg_config", return_value=evg_config):
            api_server = evergreen.get_evergreen_server()
            self.assertEqual(evergreen.DEFAULT_API_SERVER, api_server)


class TestGetEvergreenApi(unittest.TestCase):
    def test_get_evergreen_api(self):
        api_server = "https://myserver.com"
        with patch(EVERGREEN + ".get_evergreen_server", return_value=api_server):
            evg_api = evergreen.get_evergreen_api()
            self.assertEqual(api_server, evg_api.api_server)
            self.assertIsNone(evg_api.api_headers)


class TestGetHistory(unittest.TestCase):
    def test_get_history(self):
        evg_api = evergreen.EvergreenApi()
        json_data = {"history1": "val1", "history2": "val2"}
        project = "myproject"
        params = {"param1": "pval1", "param2": "pval2"}
        with patch("requests.get") as mock_req_get:
            mock_req_get.return_value.json.return_value = json_data
            history_data = evg_api.get_history(project, params)
            self.assertDictEqual(history_data, json_data)
            actual_url = mock_req_get.call_args[1]["url"]
            actual_params = mock_req_get.call_args[1]["params"]
            self.assertIn(project, actual_url)
            self.assertIn("test_history", actual_url)
            self.assertDictEqual(params, actual_params)

    def test_get_history_raise_for_status(self):
        evg_api = evergreen.EvergreenApi()
        project = "myproject"
        params = {"param1": "pval1", "param2": "pval2"}
        with patch("requests.get") as mock_req_get:
            mock_req_get.return_value.raise_for_status.side_effect = requests.exceptions.HTTPError()
            with self.assertRaises(requests.exceptions.HTTPError):
                evg_api.get_history(project, params)


class TestGetEvergreenApiV2(unittest.TestCase):
    def test_get_evergreen_api(self):
        api_server = "https://myserver.com"
        api_headers = {"header1": "val1", "header2": "val2"}
        with patch(EVERGREEN + ".get_evergreen_server", return_value=api_server),\
             patch(EVERGREEN + ".get_evergreen_headers", return_value=api_headers):
            evg_api = evergreen.get_evergreen_apiv2()
            self.assertEqual(api_server, evg_api.api_server)
            self.assertDictEqual(api_headers, evg_api.api_headers)

    def test_get_evergreen_api_kwargs(self):
        api_server = "https://myserver.com"
        api_headers = {"header1": "val1", "header2": "val2"}
        num_retries = 99
        with patch(EVERGREEN + ".get_evergreen_server", return_value=api_server),\
             patch(EVERGREEN + ".get_evergreen_headers", return_value=api_headers):
            evg_api = evergreen.get_evergreen_apiv2(num_retries=num_retries)
            self.assertEqual(api_server, evg_api.api_server)
            self.assertDictEqual(api_headers, evg_api.api_headers)

    def test_get_evergreen_api_kwargs_override(self):
        api_server = "https://myserver.com"
        api_headers = {"header1": "val1", "header2": "val2"}
        api_server_override = "https://myserver_override.com"
        api_headers_override = {"header1_override": "val1", "header2_override": "val2"}
        num_retries = 99
        with patch(EVERGREEN + ".get_evergreen_server", return_value=api_server),\
             patch(EVERGREEN + ".get_evergreen_headers", return_value=api_headers):
            with self.assertRaises(TypeError):
                evergreen.get_evergreen_apiv2(api_server=api_server_override,
                                              api_headers=api_headers_override,
                                              num_retries=num_retries)

    def test_get_evergreen_api_bad_kwargs(self):
        api_server = "https://myserver.com"
        api_headers = {"header1": "val1", "header2": "val2"}
        with patch(EVERGREEN + ".get_evergreen_server", return_value=api_server),\
             patch(EVERGREEN + ".get_evergreen_headers", return_value=api_headers):
            with self.assertRaises(TypeError):
                evergreen.get_evergreen_apiv2(kw1="kw1")


class TestCheckType(unittest.TestCase):
    def test___check_type(self):
        evergreen._check_type([1, 3], list)
        evergreen._check_type({"a": 3}, dict)
        evergreen._check_type("x", str)
        with self.assertRaises(TypeError):
            evergreen._check_type({}, list)
        with self.assertRaises(TypeError):
            evergreen._check_type([], str)


class TestAddListParam(unittest.TestCase):
    def test__add_list_param(self):
        params = {}
        param_name = "myparam"
        param_list = ["a", "b", "c"]
        evergreen._add_list_param(params, param_name, param_list)
        self.assertDictEqual({param_name: ",".join(param_list)}, params)

    def test__add_list_param_one_item(self):
        params = {}
        param_name = "myparam"
        param_list = ["abc"]
        evergreen._add_list_param(params, param_name, param_list)
        self.assertDictEqual({param_name: "abc"}, params)

    def test__add_list_param_no_list(self):
        params = {"param1": "abc"}
        param_name = "myparam"
        param_list = []
        evergreen._add_list_param(params, param_name, param_list)
        self.assertDictEqual({"param1": "abc"}, params)

    def test__add_list_param_exists(self):
        params = {"param1": "abc"}
        param_name = "param1"
        param_list = ["x", "y", "z"]
        with self.assertRaises(RuntimeError):
            evergreen._add_list_param(params, param_name, param_list)

    def test__add_list_param_extend(self):
        params = {"param1": "abc"}
        param_name = "param2"
        param_list = ["x", "y", "z"]
        evergreen._add_list_param(params, param_name, param_list)
        self.assertDictEqual({"param1": "abc", param_name: ",".join(param_list)}, params)

    def test__add_list_param_bad_type(self):
        params = {"param1": "abc"}
        param_name = "param2"
        param_list = "mystring"
        with self.assertRaises(TypeError):
            evergreen._add_list_param(params, param_name, param_list)

    def test__add_list_param_none(self):
        params = {"param1": "abc"}
        param_name = "param2"
        param_list = None
        evergreen._add_list_param(params, param_name, param_list)
        self.assertDictEqual({"param1": "abc"}, params)


class MockEvgApiV2(evergreen.EvergreenApiV2):
    def __init__(self):  #pylint: disable=super-init-not-called
        self.api_server = "http://myserver.com"
        self.session = MagicMock()


class MyTime(object):
    def __init__(self, start_time=0, increment=1):
        self.time = start_time
        self.increment = increment

    def cur_time(self):
        cur_time = self.time
        self.time += self.increment
        return cur_time


class TestEvergreenApiV2CallApi(unittest.TestCase):
    def test__call_api(self):
        url = "https://myurl.com"
        params = {}
        response = MagicMock()
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2.session.get.return_value = response
        self.assertEqual(response, mock_evgapiv2._call_api(url, params))

    def test__call_api_long_request(self):
        url = "https://myurl.com"
        params = {}
        response = MagicMock()
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2.session.get.return_value = response
        increment = 11
        mytime = MyTime(increment=increment)
        with patch("time.time", mytime.cur_time):
            self.assertEqual(response, mock_evgapiv2._call_api(url, params))

    def test__call_api_http_error(self):
        url = "https://myurl.com"
        params = {}
        response = MagicMock()
        response.raise_for_status.side_effect = requests.exceptions.HTTPError
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2.session.get.return_value = response
        with self.assertRaises(requests.exceptions.HTTPError):
            mock_evgapiv2._call_api(url, params)


class MockApi(object):
    def __init__(self, page_results):
        self._page_idx = 0
        self._page_results = page_results

    def _call_api(self, _url, _params=None):
        if self._page_idx >= len(self._page_results):
            raise requests.exceptions.HTTPError
        response = MagicMock()
        response.json = MagicMock(return_value=self._page_results[self._page_idx])
        self._page_idx += 1
        return response

    def _get_next_url(self, _):
        return None if self._page_idx == len(self._page_results) else self._page_idx


class MockPaginate(object):
    def __init__(self, page_results):
        self._page_results = page_results

    def _paginate(self, url, params=None):
        self._url = url
        self._params = params
        return list(self._paginate_gen(url, params))

    def _paginate_gen(self, url, params=None):
        self._url = url
        self._params = params
        for page_result in self._page_results:
            if isinstance(page_result, list):
                for result in page_result:
                    yield result
            else:
                yield page_result


class TestEvergreenApiV2Paginate(unittest.TestCase):
    def test___paginate_one_page_non_list(self):
        json_list = [{"key1": "val1", "key2": "val2"}]
        mock_evgapiv2 = MockEvgApiV2()
        mock_api = MockApi(json_list)
        mock_evgapiv2._call_api = mock_api._call_api
        mock_evgapiv2._get_next_url = mock_api._get_next_url
        all_json_data = mock_evgapiv2._paginate("https://myurl", None)
        self.assertEqual(len(all_json_data), 1)
        self.assertListEqual(all_json_data, json_list)
        self.assertEqual(mock_api._page_idx, 1)

    def test___paginate_multi_pages_non_list(self):
        json_list = [{"key1": "val1"}, {"key2": "val2"}]
        mock_evgapiv2 = MockEvgApiV2()
        mock_api = MockApi(json_list)
        mock_evgapiv2._call_api = mock_api._call_api
        mock_evgapiv2._get_next_url = mock_api._get_next_url
        all_json_data = mock_evgapiv2._paginate("https://myurl", None)
        self.assertEqual(len(all_json_data), 2)
        self.assertListEqual(all_json_data, json_list)
        self.assertEqual(mock_api._page_idx, 2)

    def test___paginate_one_page_multiple_items(self):
        json_list = [[{"key1": "val1"}, {"key2": "val2"}]]
        mock_evgapiv2 = MockEvgApiV2()
        mock_api = MockApi(json_list)
        mock_evgapiv2._call_api = mock_api._call_api
        mock_evgapiv2._get_next_url = mock_api._get_next_url
        all_json_data = mock_evgapiv2._paginate("https://myurl", {"param1": "myparam"})
        self.assertEqual(len(all_json_data), 2)
        self.assertListEqual(all_json_data, json_list[0])
        self.assertEqual(mock_api._page_idx, 1)

    def test___paginate_multi_pages(self):
        json_list = [[{"key1": "val1"}, {"key2": "val2"}], [{"key3": "val3"}, {"key4": "val4"}],
                     [{"key5": "val5"}]]
        mock_evgapiv2 = MockEvgApiV2()
        mock_api = MockApi(json_list)
        mock_evgapiv2._call_api = mock_api._call_api
        mock_evgapiv2._get_next_url = mock_api._get_next_url
        all_json_data = mock_evgapiv2._paginate("https://myurl", None)
        self.assertEqual(len(all_json_data), 5)
        self.assertEqual(mock_api._page_idx, 3)
        for idx, json_data in enumerate([l for sub_list in json_list for l in sub_list]):
            self.assertEqual(all_json_data[idx], json_data)


class TestEvergreenApiV2PaginateGen(unittest.TestCase):
    def test___paginate_gen_one_page(self):
        json_list = [{"key1": "val1", "key2": "val2"}]
        mock_evgapiv2 = MockEvgApiV2()
        mock_api = MockApi(json_list)
        mock_evgapiv2._call_api = mock_api._call_api
        mock_evgapiv2._get_next_url = mock_api._get_next_url
        all_json_data = list(mock_evgapiv2._paginate_gen("https://myurl", None))
        self.assertEqual(len(all_json_data), 1)
        self.assertListEqual(all_json_data, json_list)
        self.assertEqual(mock_api._page_idx, 1)

    def test___paginate_gen_multi_pages_non_list(self):
        json_list = [{"key1": "val1"}, {"key2": "val2"}]
        mock_evgapiv2 = MockEvgApiV2()
        mock_api = MockApi(json_list)
        mock_evgapiv2._call_api = mock_api._call_api
        mock_evgapiv2._get_next_url = mock_api._get_next_url
        all_json_data = list(mock_evgapiv2._paginate_gen("https://myurl", None))
        self.assertEqual(len(all_json_data), 2)
        self.assertListEqual(all_json_data, json_list)
        self.assertEqual(mock_api._page_idx, 2)

    def test___paginate_gen_multi_pages_no_response(self):
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._call_api = lambda url, params: None
        all_json_data = list(mock_evgapiv2._paginate_gen("https://myurl", None))
        self.assertEqual(len(all_json_data), 0)

    def test___paginate_gen_one_page_multiple_items(self):
        json_list = [[{"key1": "val1"}, {"key2": "val2"}]]
        mock_evgapiv2 = MockEvgApiV2()
        mock_api = MockApi(json_list)
        mock_evgapiv2._call_api = mock_api._call_api
        mock_evgapiv2._get_next_url = mock_api._get_next_url
        all_json_data = list(mock_evgapiv2._paginate_gen("https://myurl", {"param1": "myparam"}))
        self.assertEqual(len(all_json_data), 2)
        self.assertListEqual(all_json_data, json_list[0])
        self.assertEqual(mock_api._page_idx, 1)

    def test___paginate_gen_multi_pages(self):
        json_list = [[{"key1": "val1"}, {"key2": "val2"}], [{"key3": "val3"}, {"key4": "val4"}],
                     [{"key5": "val5"}]]
        mock_evgapiv2 = MockEvgApiV2()
        mock_api = MockApi(json_list)
        mock_evgapiv2._call_api = mock_api._call_api
        mock_evgapiv2._get_next_url = mock_api._get_next_url
        all_json_data = list(mock_evgapiv2._paginate_gen("https://myurl", None))
        self.assertEqual(len(all_json_data), 5)
        self.assertEqual(mock_api._page_idx, 3)
        for idx, json_data in enumerate([l for sub_list in json_list for l in sub_list]):
            self.assertEqual(all_json_data[idx], json_data)


class TestEvergreenApiV2GetNextUrl(unittest.TestCase):
    def test__get_next_url(self):
        response = Mock()
        url = "http://myurl.com?q1=a,b,c&q2=xyz"
        response.links = {"next": {"url": url}}
        self.assertEqual(evergreen.EvergreenApiV2._get_next_url(response), url)

    def test__get_next_url_no_url(self):
        response = Mock()
        response.links = {"next": {"noturl": "abc"}}
        self.assertIsNone(evergreen.EvergreenApiV2._get_next_url(response))

    def test__get_next_url_no_next(self):
        response = Mock()
        response.links = {}
        self.assertIsNone(evergreen.EvergreenApiV2._get_next_url(response))


class TestEvergreenApiV2TasksByBuildId(unittest.TestCase):
    def test_tasks_by_build_id(self):
        mock_evgapiv2 = MockEvgApiV2()
        build_id = "mybuild"
        json_list = [[{"task1": "d1", "task2": "d2"}, {"task3": "d3"}]]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        tasks = mock_evgapiv2.tasks_by_build_id(build_id)
        self.assertEqual(len(tasks), 2)
        self.assertListEqual(json_list[0], tasks)
        self.assertIn(build_id, paginate._url)

    def test_tasks_by_build_id_empty_response(self):
        mock_evgapiv2 = MockEvgApiV2()
        build_id = "mybuild"
        paginate = MockPaginate([])
        mock_evgapiv2._paginate = paginate._paginate
        tasks = mock_evgapiv2.tasks_by_build_id(build_id)
        self.assertEqual(len(tasks), 0)
        self.assertListEqual([], tasks)
        self.assertIn(build_id, paginate._url)

    def test_tasks_by_build_id_multi_page(self):
        mock_evgapiv2 = MockEvgApiV2()
        build_id = "mybuild"
        json_list = [{"task12": "d12", "task23": "d23"}, {"task356": "d34"}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        tasks = mock_evgapiv2.tasks_by_build_id(build_id)
        self.assertEqual(len(tasks), 2)
        self.assertListEqual(json_list, tasks)
        self.assertIn(build_id, paginate._url)


class TestEvergreenApiV2TestsByTask(unittest.TestCase):
    def test_tests_by_task(self):
        json_list = [{"task1": ["test1", "test2"]}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate = paginate._paginate
        task_id = "task1"
        execution = 909
        all_json_data = mock_evgapiv2.tests_by_task(task_id, execution)
        self.assertListEqual(json_list, all_json_data)
        self.assertIn(task_id, paginate._url)
        self.assertIn("execution", paginate._params)
        self.assertEqual(paginate._params["execution"], execution)

    def test_tests_by_task_limit(self):
        json_list = [{"task2": ["test1a", "test2c"]}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate = paginate._paginate
        task_id = "task1"
        execution = 209
        limit = 50
        all_json_data = mock_evgapiv2.tests_by_task(task_id, execution, limit=limit)
        self.assertListEqual(json_list, all_json_data)
        self.assertIn(task_id, paginate._url)
        self.assertIn("execution", paginate._params)
        self.assertEqual(paginate._params["execution"], execution)
        self.assertIn("limit", paginate._params)
        self.assertEqual(paginate._params["limit"], limit)

    def test_tests_by_task_status(self):
        json_list = [{"task1b": ["test13", "test27"]}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate = paginate._paginate
        task_id = "task1"
        execution = 75
        limit = 250
        status = "passed"
        all_json_data = mock_evgapiv2.tests_by_task(task_id, execution, limit=limit, status=status)
        self.assertListEqual(json_list, all_json_data)
        self.assertIn(task_id, paginate._url)
        self.assertIn("execution", paginate._params)
        self.assertEqual(paginate._params["execution"], execution)
        self.assertIn("limit", paginate._params)
        self.assertEqual(paginate._params["limit"], limit)
        self.assertIn("status", paginate._params)
        self.assertEqual(paginate._params["status"], status)


class TestEvergreenApiV2ProjectPatchesGen(unittest.TestCase):
    def test_project_patches_gen(self):
        json_list = [[{"create_time": "2019-01-01", "data": "mydata1"},
                      {"create_time": "2018-12-01", "data": "mydata2"}]]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate_gen = paginate._paginate_gen
        project = "myproject"
        all_json_data = list(mock_evgapiv2.project_patches_gen(project))
        self.assertEqual(len(all_json_data), 2)
        self.assertListEqual(json_list[0], all_json_data)
        self.assertIn(project, paginate._url)

    def test_project_patches_gen_days_limit(self):
        limit = 1
        json_list = [[{"create_time": "2016-01-01", "data": "mydata1"},
                      {"create_time": "2017-12-01", "data": "mydata2"},
                      {"create_time": "2018-01-01", "data": "mydata3"}]]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate_gen = paginate._paginate_gen
        project = "myproject2"
        all_json_data = list(mock_evgapiv2.project_patches_gen(project, limit=limit))
        self.assertEqual(len(all_json_data), 3)
        for idx, json_data in enumerate(all_json_data):
            self.assertDictEqual(json_list[0][idx], json_data)
        self.assertIn("limit", paginate._params)
        self.assertIn(project, paginate._url)
        self.assertEqual(paginate._params["limit"], limit)

    def test_project_patches_gen_multi_page(self):
        json_list = [{"create_time": "2015-01-01", "data": "mydata12"},
                     {"create_time": "2016-12-01", "data": "mydata22"}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate_gen = paginate._paginate_gen
        project = "myproject3"
        all_json_data = list(mock_evgapiv2.project_patches_gen(project))
        self.assertEqual(len(all_json_data), 2)
        self.assertDictEqual(json_list[0], all_json_data[0])
        self.assertIn(project, paginate._url)

    def test_project_patches_gen_no_results(self):
        json_list = [[]]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate_gen = paginate._paginate_gen
        project = "myproject4"
        all_json_data = list(mock_evgapiv2.project_patches_gen(project))
        self.assertEqual(len(all_json_data), 0)
        self.assertIn(project, paginate._url)


class TestEvergreenApiV2VersionBuilds(unittest.TestCase):
    def test_version_builds(self):
        json_list = [[{"build1": {"build_data": ["a.js"]}}, {"build2": {"build_data": ["b.js"]}}]]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate = paginate._paginate
        version = "version1"
        all_json_data = mock_evgapiv2.version_builds(version)
        self.assertEqual(len(all_json_data), 2)
        for idx, json_data in enumerate(all_json_data):
            self.assertDictEqual(json_list[0][idx], json_data)
        self.assertIn(version, paginate._url)
        self.assertIsNone(paginate._params)

    def test_version_builds_gen_multi_page(self):
        json_list = [[{"build1": {"build_data": ["a.js"]}}, {"build2": {"build_data": ["b.js"]}}],
                     [{"build3": {"build_data": ["c.js"]}}]]
        paginate = MockPaginate(json_list)
        mock_evgapiv2 = MockEvgApiV2()
        mock_evgapiv2._paginate = paginate._paginate
        version = "version1"
        all_json_data = mock_evgapiv2.version_builds(version)
        self.assertEqual(len(all_json_data), 3)
        json_data_list = [l for sub_list in json_list for l in sub_list]
        for idx, json_data in enumerate(all_json_data):
            self.assertDictEqual(json_data_list[idx], json_data)
        self.assertIn(version, paginate._url)
        self.assertIsNone(paginate._params)


class TestEvergreenApiV2TaskOrTestStats(unittest.TestCase):
    def test__task_or_test_stats(self):
        mock_evgapiv2 = MockEvgApiV2()
        endpoint_name = "test_stats"
        project = "myproject"
        after_date = "2019-01-01"
        before_date = "2019-02-01"
        json_list = [{"stat1": {"data": "val1"}}, {"stat2": {"data": "val2"}}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        stats = mock_evgapiv2._task_or_test_stats(endpoint_name, project, after_date, before_date)
        self.assertEqual(2, len(stats))
        self.assertIn(endpoint_name, paginate._url)
        self.assertEqual(after_date, paginate._params["after_date"])
        self.assertEqual(before_date, paginate._params["before_date"])
        self.assertEqual(mock_evgapiv2.DEFAULT_SORT, paginate._params["sort"])
        self.assertEqual(mock_evgapiv2.DEFAULT_LIMIT, paginate._params["limit"])
        self.assertEqual(mock_evgapiv2.DEFAULT_GROUP_NUM_DAYS, paginate._params["group_num_days"])
        self.assertEqual(",".join(mock_evgapiv2.DEFAULT_REQUESTERS), paginate._params["requesters"])
        for unspecified_param in ["tests", "tasks", "variants", "distros", "group_by"]:
            self.assertNotIn(unspecified_param, paginate._params)

    def test__task_or_test_stats_all_params(self):  #pylint: disable=too-many-locals
        mock_evgapiv2 = MockEvgApiV2()
        endpoint_name = "test_stats"
        project = "myproject"
        after_date = "2018-01-01"
        before_date = "2018-02-01"
        group_num_days = 3
        requesters = ["requester1", "requester2"]
        sort = "sortup"
        limit = 34
        tests = ["test1", "test2"]
        tasks = ["task1", "task2"]
        variants = ["variant1", "variant2"]
        distros = ["distro1", "distro2"]
        group_by = "mygroup"
        json_list = [{"stat1": {"data": "val1"}}, {"stat2": {"data": "val2"}}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        stats = mock_evgapiv2._task_or_test_stats(
            endpoint_name, project, after_date, before_date, group_num_days=group_num_days,
            requesters=requesters, sort=sort, limit=limit, tests=tests, tasks=tasks,
            variants=variants, distros=distros, group_by=group_by)
        self.assertEqual(2, len(stats))
        self.assertIn(endpoint_name, paginate._url)
        self.assertEqual(after_date, paginate._params["after_date"])
        self.assertEqual(before_date, paginate._params["before_date"])
        self.assertEqual(group_num_days, paginate._params["group_num_days"])
        self.assertEqual(",".join(requesters), paginate._params["requesters"])
        self.assertEqual(sort, paginate._params["sort"])
        self.assertEqual(limit, paginate._params["limit"])
        self.assertEqual(",".join(tests), paginate._params["tests"])
        self.assertEqual(",".join(tasks), paginate._params["tasks"])
        self.assertEqual(",".join(variants), paginate._params["variants"])
        self.assertEqual(",".join(distros), paginate._params["distros"])
        self.assertEqual(group_by, paginate._params["group_by"])

    def test__task_or_test_stats_bad_endpoint(self):
        mock_evgapiv2 = MockEvgApiV2()
        endpoint_name = "no_endpoint"
        project = "myproject"
        after_date = "2019-01-01"
        before_date = "2019-02-01"
        json_list = [{"stat1": {"data": "val1"}}, {"stat2": {"data": "val2"}}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        with self.assertRaises(ValueError):
            mock_evgapiv2._task_or_test_stats(endpoint_name, project, after_date, before_date)

    def test__task_or_test_stats_tasks_with_tests(self):
        mock_evgapiv2 = MockEvgApiV2()
        endpoint_name = "task_stats"
        project = "myproject"
        after_date = "2019-01-01"
        before_date = "2019-02-01"
        tests = ["test1", "test2"]
        json_list = [{"stat1": {"data": "val1"}}, {"stat2": {"data": "val2"}}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        with self.assertRaises(ValueError):
            mock_evgapiv2._task_or_test_stats(endpoint_name, project, after_date, before_date,
                                              tests=tests)


class TestEvergreenApiV2TestStats(unittest.TestCase):
    def test_test_stats(self):
        mock_evgapiv2 = MockEvgApiV2()
        endpoint_name = "test_stats"
        project = "myproject"
        after_date = "2019-01-01"
        before_date = "2019-02-01"
        json_list = [{"stat1": {"data": "val1"}}, {"stat2": {"data": "val2"}}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        stats = mock_evgapiv2.test_stats(project, after_date, before_date)
        self.assertEqual(2, len(stats))
        self.assertIn(endpoint_name, paginate._url)
        self.assertEqual(after_date, paginate._params["after_date"])
        self.assertEqual(before_date, paginate._params["before_date"])
        self.assertEqual(mock_evgapiv2.DEFAULT_SORT, paginate._params["sort"])
        self.assertEqual(mock_evgapiv2.DEFAULT_LIMIT, paginate._params["limit"])
        self.assertEqual(mock_evgapiv2.DEFAULT_GROUP_NUM_DAYS, paginate._params["group_num_days"])
        self.assertEqual(",".join(mock_evgapiv2.DEFAULT_REQUESTERS), paginate._params["requesters"])
        for unspecified_param in ["tests", "tasks", "variants", "distros", "group_by"]:
            self.assertNotIn(unspecified_param, paginate._params)

    def test_test_stats_all_params(self):  #pylint: disable=too-many-locals
        mock_evgapiv2 = MockEvgApiV2()
        endpoint_name = "test_stats"
        project = "myproject"
        after_date = "2019-01-01"
        before_date = "2019-02-01"
        group_num_days = 3
        requesters = ["requester1", "requester2"]
        sort = "sortup"
        limit = 34
        tests = ["test1", "test2"]
        tasks = ["task1", "task2"]
        variants = ["variant1", "variant2"]
        distros = ["distro1", "distro2"]
        group_by = "mygroup"
        json_list = [{"stat1": {"data": "val1"}}, {"stat2": {"data": "val2"}}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        stats = mock_evgapiv2.test_stats(project, after_date, before_date,
                                         group_num_days=group_num_days, requesters=requesters,
                                         sort=sort, limit=limit, tests=tests, tasks=tasks,
                                         variants=variants, distros=distros, group_by=group_by)
        self.assertEqual(2, len(stats))
        self.assertIn(endpoint_name, paginate._url)
        self.assertEqual(after_date, paginate._params["after_date"])
        self.assertEqual(before_date, paginate._params["before_date"])
        self.assertEqual(group_num_days, paginate._params["group_num_days"])
        self.assertEqual(",".join(requesters), paginate._params["requesters"])
        self.assertEqual(sort, paginate._params["sort"])
        self.assertEqual(limit, paginate._params["limit"])
        self.assertEqual(",".join(tests), paginate._params["tests"])
        self.assertEqual(",".join(tasks), paginate._params["tasks"])
        self.assertEqual(",".join(variants), paginate._params["variants"])
        self.assertEqual(",".join(distros), paginate._params["distros"])
        self.assertEqual(group_by, paginate._params["group_by"])


class TestEvergreenApiV2TaskStats(unittest.TestCase):
    def test_task_stats(self):
        mock_evgapiv2 = MockEvgApiV2()
        endpoint_name = "task_stats"
        project = "myproject"
        after_date = "2019-01-01"
        before_date = "2019-02-01"
        json_list = [{"stat1": {"data": "val1"}}, {"stat2": {"data": "val2"}}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        stats = mock_evgapiv2.task_stats(project, after_date, before_date)
        self.assertEqual(2, len(stats))
        self.assertIn(endpoint_name, paginate._url)
        self.assertEqual(after_date, paginate._params["after_date"])
        self.assertEqual(before_date, paginate._params["before_date"])
        self.assertEqual(mock_evgapiv2.DEFAULT_SORT, paginate._params["sort"])
        self.assertEqual(mock_evgapiv2.DEFAULT_LIMIT, paginate._params["limit"])
        self.assertEqual(mock_evgapiv2.DEFAULT_GROUP_NUM_DAYS, paginate._params["group_num_days"])
        self.assertEqual(",".join(mock_evgapiv2.DEFAULT_REQUESTERS), paginate._params["requesters"])
        for unspecified_param in ["tasks", "variants", "distros", "group_by"]:
            self.assertNotIn(unspecified_param, paginate._params)

    def test_test_stats_all_params(self):  #pylint: disable=too-many-locals
        mock_evgapiv2 = MockEvgApiV2()
        endpoint_name = "task_stats"
        project = "myproject"
        after_date = "2019-01-01"
        before_date = "2019-02-01"
        group_num_days = 3
        requesters = ["requester1", "requester2"]
        sort = "sortup"
        limit = 34
        tasks = ["task1", "task2"]
        variants = ["variant1", "variant2"]
        distros = ["distro1", "distro2"]
        group_by = "mygroup"
        json_list = [{"stat1": {"data": "val1"}}, {"stat2": {"data": "val2"}}]
        paginate = MockPaginate(json_list)
        mock_evgapiv2._paginate = paginate._paginate
        stats = mock_evgapiv2.task_stats(project, after_date, before_date,
                                         group_num_days=group_num_days, requesters=requesters,
                                         sort=sort, limit=limit, tasks=tasks, variants=variants,
                                         distros=distros, group_by=group_by)
        self.assertEqual(2, len(stats))
        self.assertIn(endpoint_name, paginate._url)
        self.assertEqual(after_date, paginate._params["after_date"])
        self.assertEqual(before_date, paginate._params["before_date"])
        self.assertEqual(group_num_days, paginate._params["group_num_days"])
        self.assertEqual(",".join(requesters), paginate._params["requesters"])
        self.assertEqual(sort, paginate._params["sort"])
        self.assertEqual(limit, paginate._params["limit"])
        self.assertEqual(",".join(tasks), paginate._params["tasks"])
        self.assertEqual(",".join(variants), paginate._params["variants"])
        self.assertEqual(",".join(distros), paginate._params["distros"])
        self.assertEqual(group_by, paginate._params["group_by"])
