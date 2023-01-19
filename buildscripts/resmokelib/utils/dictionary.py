"""Utility functions for working with Dict-type structures."""
from typing import MutableMapping


def merge_dicts(dict1, dict2):
    """Recursively merges dict2 into dict1."""
    if not (isinstance(dict1, MutableMapping) and isinstance(dict2, MutableMapping)):
        return dict2

    for k in dict2.keys():
        if dict2[k] is None:
            dict1.pop(k)
        elif k in dict1:
            dict1[k] = merge_dicts(dict1[k], dict2[k])
        else:
            dict1[k] = dict2[k]
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
        if key not in path:
            current_object[key] = {}

        current_object = current_object[key]

    current_object[path[-1]] = value
