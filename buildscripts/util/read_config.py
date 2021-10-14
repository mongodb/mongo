"""Functions to read configuration files."""
import yaml


def get_config_value(attrib, cmd_line_options, config_file_data, required=False, default=None):
    """
    Get the configuration value to use.

    First use command line options, then config file option, then the default. If required is
    true, throw an exception if the value is not found.

    :param attrib: Attribute to search for.
    :param cmd_line_options: Command line options.
    :param config_file_data: Config file data.
    :param required: Is this option required.
    :param default: Default value if option is not found.
    :return: value to use for this option.
    """
    value = getattr(cmd_line_options, attrib, None)
    if value is not None:
        return value

    if attrib in config_file_data:
        return config_file_data[attrib]

    if required:
        raise KeyError("{0} must be specified".format(attrib))

    return default


def read_config_file(config_file):
    """
    Read the yaml config file specified.

    :param config_file: path to config file.
    :return: Object representing contents of config file.
    """
    config_file_data = {}
    if config_file:
        with open(config_file) as file_handle:
            config_file_data = yaml.safe_load(file_handle)

    return config_file_data
