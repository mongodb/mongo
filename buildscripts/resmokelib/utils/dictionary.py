"""Utility functions for working with Dict-type structures."""

from typing import MutableMapping


def merge_dicts(dict1, dict2):
    """Recursively merges dict2 into dict1."""
    if not (isinstance(dict1, MutableMapping) and isinstance(dict2, MutableMapping)):
        return dict2

    for k in dict2.keys():
        if dict2[k] is None:
            if k in dict1:
                dict1.pop(k)
        elif k in dict1:
            dict1[k] = merge_dicts(dict1[k], dict2[k])
        else:
            dict1[k] = dict2[k]
    return dict1


def extend_dict_lists(dict1, dict2):
    """Recursively merges dict2 into dict1, by extending the lists on dict1.

    All terminal elements in dict2 must be lists. For each terminal element in dict2, the matching
    path must already exist in dict1, and the element must be a list.

    -- Example --
    [dict1 contents]
    root:
        child:
            some_key:
            - element 1
            - element 2

    [dict2 contents]
    root:
        child:
            some_key:
                - element 3
                - element 4

    [result]
    root:
        child:
            some_key:
                - element 1
                - element 2
                - element 3
                - element 4
    """

    def assert_valid_instance(obj):
        if not isinstance(obj, (list, MutableMapping)):
            raise ValueError(f"the {obj} field must be a list")

    if not (isinstance(dict1, MutableMapping) and isinstance(dict2, MutableMapping)):
        if not isinstance(dict1, list):
            raise ValueError(f"{dict1} must be a list")

        if not isinstance(dict2, list):
            raise ValueError(f"{dict2} must be a list")

        dict1.extend(dict2)
        return dict1

    for k in dict2.keys():
        if k not in dict1:
            raise ValueError(f"the {k} field must be present of both dicts")

        assert_valid_instance(dict2[k])
        assert_valid_instance(dict1[k])

        dict1[k] = extend_dict_lists(dict1[k], dict2[k])

    return dict1


def get_dict_value(dict1, path):
    current_object = dict1

    for key in path:
        if key not in current_object:
            return None

        current_object = current_object[key]

    return current_object


def set_dict_value(dict1, path, value):
    current_object = dict1

    for key in path[:-1]:
        if key not in current_object:
            current_object[key] = {}

        current_object = current_object[key]

    current_object[path[-1]] = value
