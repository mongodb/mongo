"""Unit tests for buildscripts/evergreen_activate_gen_tasks.py."""

import http.client
import json
import unittest
import urllib.error
from io import BytesIO

from mock import patch

import buildscripts.evergreen_activate_gen_tasks as under_test

ENV = {
    "build_id": "build_id",
    "version_id": "version_id",
    "task_name": "auth_gen",
    "evergreen_api_user": "user",
    "evergreen_api_key": "key",
}


class MockResponse(BytesIO):
    """Enough of an http.client.HTTPResponse for urlopen's context manager."""

    def __init__(self, body, link=None):
        super().__init__(json.dumps(body).encode())
        self.headers = {"Link": link}

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def build_task(display_name, task_id):
    return {"display_name": display_name, "task_id": task_id}


class FakeUrlopen:
    """Records requests and replays queued responses, so no network is involved."""

    def __init__(self, pages, patch_error=None):
        self.pages = list(pages)
        self.patch_error = patch_error
        self.requests = []

    def __call__(self, request, *args, **kwargs):
        self.requests.append(request)
        if request.method == "PATCH":
            if self.patch_error:
                raise self.patch_error
            return MockResponse({})
        return self.pages.pop(0)

    @property
    def activated_task_ids(self):
        return [r.full_url.rsplit("/", 1)[-1] for r in self.requests if r.method == "PATCH"]


def run(fake, env_overrides=None):
    expansions = under_test.EvgExpansions.from_environ({**ENV, **(env_overrides or {})})
    api = under_test.SimpleEvergreenApi(expansions)
    with (
        patch.object(under_test.urllib.request, "urlopen", fake),
        patch.object(under_test.time, "sleep"),
    ):
        return under_test.activate_task(expansions, api)


class TestActivateTask(unittest.TestCase):
    def test_task_matching_the_gen_task_name_is_activated(self):
        fake = FakeUrlopen(
            [MockResponse([build_task("auth_gen", "id_gen"), build_task("auth", "id_auth")])]
        )

        run(fake)
        self.assertEqual(fake.activated_task_ids, ["id_auth"])

    def test_no_matching_task_is_not_an_error(self):
        """A patch that did not schedule the task, or a suite with no tests, generates no task."""
        fake = FakeUrlopen([MockResponse([build_task("something_else", "id_0")])])

        run(fake)
        self.assertEqual(fake.activated_task_ids, [])

    def test_failure_to_activate_an_existing_task_fails_the_task(self):
        fake = FakeUrlopen(
            [MockResponse([build_task("auth", "id_auth")])],
            patch_error=urllib.error.HTTPError("url", 403, "Forbidden", {}, None),
        )

        with self.assertRaises(ValueError):
            run(fake)

    def test_burn_in_tests_gen_activates_burn_in_tests(self):
        fake = FakeUrlopen([MockResponse([build_task("burn_in_tests", "id_burn_in")])])

        run(fake, {"task_name": "burn_in_tests_gen"})
        self.assertEqual(fake.activated_task_ids, ["id_burn_in"])

    def test_every_page_of_tasks_is_searched(self):
        next_link = (
            '<https://evergreen.mongodb.com/api/rest/v2/builds/b/tasks?start_at=p2>; rel="next"'
        )
        fake = FakeUrlopen(
            [
                MockResponse([build_task("noise", "id_0")], link=next_link),
                MockResponse([build_task("auth", "id_auth")]),
            ]
        )

        run(fake)
        self.assertEqual(fake.activated_task_ids, ["id_auth"])

    def test_endless_pagination_is_an_error_rather_than_an_empty_result(self):
        """Truncating the walk would look exactly like the generated task not existing."""
        endless = (
            '<https://evergreen.mongodb.com/api/rest/v2/builds/b/tasks?start_at=x>; rel="next"'
        )
        fake = FakeUrlopen(
            [MockResponse([build_task("noise", "id_0")], link=endless)] * under_test.MAX_PAGES
        )

        with self.assertRaises(RuntimeError):
            run(fake)

    def test_credentials_are_sent_on_every_request(self):
        fake = FakeUrlopen([MockResponse([build_task("auth", "id_auth")])])

        run(fake)

        for request in fake.requests:
            self.assertEqual(request.get_header("Api-user"), "user")
            self.assertEqual(request.get_header("Api-key"), "key")


class TestRetries(unittest.TestCase):
    def test_a_server_error_is_retried_then_succeeds(self):
        attempts = []

        def flaky(request, *args, **kwargs):
            if request.method != "GET":
                return MockResponse({})
            attempts.append(request)
            if len(attempts) == 1:
                raise urllib.error.HTTPError("url", 503, "Service Unavailable", {}, None)
            return MockResponse([build_task("auth", "id_auth")])

        run(FakeUrlopenWrapper(flaky))
        self.assertEqual(len(attempts), 2, "the GET is retried once and then succeeds")

    def test_a_client_error_is_retried_then_raised(self):
        attempts = []

        def unauthorized(request, *args, **kwargs):
            attempts.append(request)
            raise urllib.error.HTTPError("url", 401, "Unauthorized", {}, None)

        with self.assertRaises(urllib.error.HTTPError):
            run(FakeUrlopenWrapper(unauthorized))
        self.assertEqual(len(attempts), under_test.RETRY_ATTEMPTS)

    def test_a_body_cut_off_mid_read_is_retried(self):
        """The failure the previous implementation retried as a ChunkedEncodingError."""
        attempts = []

        def truncated(request, *args, **kwargs):
            if request.method != "GET":
                return MockResponse({})
            attempts.append(request)
            if len(attempts) == 1:
                raise http.client.IncompleteRead(b"half a page")
            return MockResponse([build_task("auth", "id_auth")])

        run(FakeUrlopenWrapper(truncated))
        self.assertEqual(len(attempts), 2)

    def test_persistent_server_errors_give_up_and_raise(self):
        attempts = []

        def always_failing(request, *args, **kwargs):
            attempts.append(request)
            raise urllib.error.URLError("connection reset")

        with self.assertRaises(urllib.error.URLError):
            run(FakeUrlopenWrapper(always_failing))
        self.assertEqual(len(attempts), under_test.RETRY_ATTEMPTS)


class FakeUrlopenWrapper:
    """Adapts a plain callable to the FakeUrlopen interface used by run()."""

    def __init__(self, func):
        self.func = func
        self.requests = []

    def __call__(self, request, *args, **kwargs):
        self.requests.append(request)
        return self.func(request, *args, **kwargs)

    @property
    def activated_task_ids(self):
        return [r.full_url.rsplit("/", 1)[-1] for r in self.requests if r.method == "PATCH"]


def version_doc(*variants):
    """A '/versions/<id>' response listing the given (build_variant, build_id) pairs."""
    return MockResponse(
        {
            "build_variants_status": [
                {"build_variant": name, "build_id": build_id} for name, build_id in variants
            ]
        }
    )


class TestBurnInTags(unittest.TestCase):
    """'burn_in_tags_gen' generates 'burn_in_tests' tasks into other variants of the version."""

    def test_burn_in_tests_is_activated_in_every_generated_variant(self):
        fake = FakeUrlopen(
            [
                version_doc(
                    ("variant1-generated-by-burn-in-tags", "b1"),
                    ("variant2-generated-by-burn-in-tags", "b2"),
                ),
                MockResponse([build_task("burn_in_tests", "id_1")]),
                MockResponse([build_task("burn_in_tests", "id_2")]),
            ]
        )

        run(fake, {"task_name": "burn_in_tags_gen"})
        self.assertEqual(fake.activated_task_ids, ["id_1", "id_2"])

    def test_a_variant_with_no_burn_in_task_contributes_nothing(self):
        fake = FakeUrlopen(
            [
                version_doc(
                    ("variant1-non-burn-in", "b1"),
                    ("variant2-generated-by-burn-in-tags", "b2"),
                ),
                MockResponse([build_task("burn_in_tags_gen", "id_1")]),
                MockResponse([build_task("burn_in_tests", "id_2")]),
            ]
        )

        run(fake, {"task_name": "burn_in_tags_gen"})
        self.assertEqual(fake.activated_task_ids, ["id_2"])

    def test_variants_not_generated_by_burn_in_tags_activate_burn_in_tests_by_prefix(self):
        """A variant that burn_in_tags did not generate can hold e.g. 'burn_in_tests_multiversion'."""
        fake = FakeUrlopen(
            [
                version_doc(("variant1-non-burn-in", "b1")),
                MockResponse(
                    [
                        build_task("burn_in_tests_multiversion", "id_multiversion"),
                        build_task("auth", "id_auth"),
                    ]
                ),
            ]
        )

        run(fake, {"task_name": "burn_in_tags_gen"})
        self.assertEqual(fake.activated_task_ids, ["id_multiversion"])

    def test_no_burn_in_task_anywhere_in_the_version_is_not_an_error(self):
        fake = FakeUrlopen(
            [
                version_doc(("variant1-non-burn-in", "b1")),
                MockResponse([build_task("auth", "id_auth")]),
            ]
        )

        run(fake, {"task_name": "burn_in_tags_gen"})
        self.assertEqual(fake.activated_task_ids, [])


class TestEvgExpansions(unittest.TestCase):
    def test_missing_expansions_are_reported(self):
        with self.assertRaises(KeyError) as err:
            under_test.EvgExpansions.from_environ({"build_id": "b"})

        self.assertIn("version_id", str(err.exception))
        self.assertIn("task_name", str(err.exception))
        self.assertIn("evergreen_api_key", str(err.exception))

    def test_api_server_defaults_but_can_be_overridden(self):
        self.assertEqual(
            under_test.EvgExpansions.from_environ(ENV).api_server, under_test.DEFAULT_API_SERVER
        )
        self.assertEqual(
            under_test.EvgExpansions.from_environ(
                {**ENV, "evergreen_api_server": "https://example.com/api"}
            ).api_server,
            "https://example.com/api",
        )


class TestRemoveGenSuffix(unittest.TestCase):
    def test_suffix_is_removed(self):
        self.assertEqual(under_test.remove_gen_suffix("auth_gen"), "auth")

    def test_name_without_the_suffix_is_unchanged(self):
        self.assertEqual(under_test.remove_gen_suffix("auth"), "auth")

    def test_only_the_trailing_suffix_is_removed(self):
        self.assertEqual(under_test.remove_gen_suffix("gen_auth_gen"), "gen_auth")


class TestNextPageUrl(unittest.TestCase):
    def test_no_link_header_is_the_last_page(self):
        self.assertIsNone(under_test._next_page_url(None))

    def test_next_link_is_returned(self):
        self.assertEqual(
            under_test._next_page_url('<https://x/tasks?p=2>; rel="next"'), "https://x/tasks?p=2"
        )

    def test_next_is_picked_out_of_several_relations(self):
        header = '<https://x/tasks?p=1>; rel="prev", <https://x/tasks?p=3>; rel="next"'

        self.assertEqual(under_test._next_page_url(header), "https://x/tasks?p=3")

    def test_a_last_page_with_only_a_prev_link_stops_the_walk(self):
        self.assertIsNone(under_test._next_page_url('<https://x/tasks?p=1>; rel="prev"'))


if __name__ == "__main__":
    unittest.main()
