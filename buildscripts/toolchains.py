#!/usr/bin/env python3
"""Provides data regarding mongodbtoolchain."""

import argparse
import enum
import os
import pathlib
import string
import sys
import warnings

from typing import Any, Callable, Dict, Generator, Iterator, List, Mapping
from typing import Optional, Set, Sequence, Tuple, Union

import yaml

__all__ = [
    'DEFAULT_DATA_FILE',
    'Toolchain',
    'ToolchainConfig',
    'ToolchainDataException',
    'ToolchainReleaseName',
    'ToolchainVersionName',
    'Toolchains',
]

DEFAULT_DATA_FILE: pathlib.Path = pathlib.Path(__file__).parent / '../etc/toolchains.yaml'


class ToolchainVersionName(str, enum.Enum):
    """Represents a "named" toolchain version, such as "stable" or "testing"."""

    STABLE = 'stable'
    TESTING = 'testing'

    # pylint: disable=invalid-str-returned
    def __str__(self) -> str:
        return self.value


class ToolchainReleaseName(str, enum.Enum):
    """Represents a "named" toolchain release, such as "rollback" or "current"."""

    ROLLBACK = 'rollback'
    CURRENT = 'current'
    LATEST = 'latest'

    # pylint: disable=invalid-str-returned
    def __str__(self) -> str:
        return self.value


class ToolchainDistroName(Tuple[str, ...], enum.Enum):
    """Represents a distribution for which the toolchain is built."""

    AMAZON1_2012 = (
        'amazon1-2012',
        'linux-64-amzn',
    )
    AMAZON1_2018 = ('amazon1-2018', )
    AMAZON2 = ('amazon2', )
    ARCHLINUX = ('archlinux', )
    CENTOS6 = ('centos6', )
    DEBIAN8 = ('debian81', )
    DEBIAN9 = ('debian92', )
    DEBIAN10 = ('debian10', )
    DEBIAN11 = ('debian11', )
    MACOS1012 = ('macos-1012', )
    MACOS1014 = ('macos-1014', )
    MACOS1100 = ('macos-1100', )
    RHEL6 = ('rhel6', 'rhel62', 'rhel67')
    RHEL7 = ('rhel7', 'rhel70', 'rhel71', 'rhel72', 'rhel76', 'ubi7')
    RHEL8 = ('rhel8', 'rhel80', 'rhel81', 'rhel82', 'rhel83', 'rhel84', 'ubi8')
    SUSE12 = ('suse12', 'suse12-sp5')
    SUSE15 = ('suse15', 'suse15-sp0', 'suse15-sp2')
    UBUNTU1404 = ('ubuntu1404', )
    UBUNTU1604 = ('ubuntu1604', )
    UBUNTU1804 = ('ubuntu1804', )
    UBUNTU2004 = ('ubuntu2004', )
    DEFAULT = ('default', )

    @classmethod
    def from_str(cls, text: str) -> 'ToolchainDistroName':
        """Return the enumeration object matching a given string."""

        for distro in cls:
            if text in distro.value:
                return distro

        raise ValueError(f"Unknown distro string: {text}")

    # pylint: disable=unsubscriptable-object
    def __str__(self) -> str:
        return self.value[0]


class ToolchainArchName(Tuple[str, ...], enum.Enum):
    """Represents an architecture for which the toolchain is built."""

    ARM64 = ('arm64', 'aarch64')
    PPC64LE = ('ppc64le', 'power8')
    S390X = ('s390x', 'zSeries')
    X86_64 = ('x86_64', )
    DEFAULT = ('', )

    @classmethod
    def from_str(cls, text: str) -> 'ToolchainArchName':
        """Return the enumeratrion object matching a given string."""

        for arch in cls:
            if text in arch.value:
                return arch

        raise ValueError(f"Unknown arch string: {text}")

    # pylint: disable=unsubscriptable-object
    def __str__(self) -> str:
        return self.value[0]


class ToolchainDataException(Exception):
    """Represents a problem encountered while reading or querying toolchain data."""


class ToolchainPlatform:
    """Represents a platform for which the toolchain is built."""

    def __init__(self, distro_id: str, arch: Optional[ToolchainArchName] = None) -> None:
        """Parse a distro_id into a full toolchain platform."""

        self._distro_id: str = distro_id

        self._distro: Optional[ToolchainDistroName] = None
        self._arch: Optional[ToolchainArchName] = arch
        self._tag: Optional[str] = None

        # These two actions are order-dependent!
        self._distro_length: int = self._find_distro_length()
        self._arch_span: Tuple[int, int] = self._find_arch_span()

    def _split_distro_id(self, start: int = 0) -> Tuple[str, str]:
        return self._distro_id[start:].split('-', 1)[0], self._distro_id[start:].split('.')[0]

    def _find_distro_length(self) -> int:
        for distro in ToolchainDistroName:
            for name in distro.value:

                if not name:
                    continue

                if name.lower() in self._split_distro_id():
                    return len(name)

        raise ValueError(f"Failed to match distro from distro_id: `{self._distro_id}'")

    def _find_arch_span(self) -> Tuple[int, int]:
        arch_span: Optional[Tuple[int, int]] = None

        for arch in ToolchainArchName:
            for name in arch.value:

                if not name:
                    continue

                iter_start = self._distro_length + 1
                while iter_start < len(self._distro_id):

                    if name.lower() in self._split_distro_id(self._distro_length):
                        arch_span = (iter_start, len(name))

                    iter_start += len(name)
                    if iter_start < len(self._distro_id) and self._distro_id[iter_start] in ('-',
                                                                                             '.'):
                        iter_start += 1

        if arch_span is None:
            arch_span = (self._distro_length, 0)

        return arch_span

    @property
    def distro_id(self) -> str:
        """Return the distro_id."""

        return self._distro_id

    @property
    def distro(self) -> ToolchainDistroName:
        """Return the distro component of the distro_id."""

        if self._distro is None:
            distro_length: int = self._distro_length
            self._distro = ToolchainDistroName.from_str(self._distro_id[:distro_length])

        return self._distro

    @property
    def arch(self) -> ToolchainArchName:
        """Return the architecture component of the distro_id."""

        if self._arch is None:
            arch_span: Tuple[int, int] = self._arch_span
            if arch_span[1] == 0:
                # There is no architecture specified in the distro_id
                if self.distro == ToolchainDistroName.DEFAULT:
                    # The "default" distro is allowed to have a nonexistent arch
                    self._arch = ToolchainArchName.DEFAULT
                else:
                    self._arch = ToolchainArchName.X86_64
            else:
                self._arch = ToolchainArchName.from_str(
                    self.distro_id[arch_span[0]:arch_span[0] + arch_span[1]])

        return self._arch

    @property
    def tag(self) -> Optional[str]:
        """Return the descriptive tag component of the distro_id."""

        if self._tag is None:
            arch_span: Tuple[int, int] = self._arch_span
            if arch_span[0] + arch_span[1] + 1 < len(self._distro_id):
                self._tag = self._distro_id[arch_span[0] + arch_span[1] + 1:]
            else:
                self._tag = ''

        if self._tag:
            return self._tag

        return None

    def __str__(self) -> str:
        result: str = f"{self.distro}"

        if self.arch != ToolchainArchName.DEFAULT:
            result += f".{self.arch}"

        if self.tag is not None:
            result += f".{self.tag}"

        return result


class ToolchainConfig:
    """Represents a toolchain configuration."""

    def __init__(self, data_file: pathlib.Path, platform: ToolchainPlatform) -> None:
        """Construct a toolchain configuration from a data file."""

        try:
            with open(data_file.absolute(), 'r', encoding='utf-8') as yaml_stream:
                self._data = yaml.safe_load(yaml_stream)
        except yaml.YAMLError as parent_exc:
            raise ToolchainDataException(
                f"Could not read toolchain data file: `{data_file}'") from parent_exc

        self._platform: ToolchainPlatform = platform

    @property
    def base_path(self) -> pathlib.Path:
        """Return the base (installed) path for toolchain releases."""

        return pathlib.Path(self._data['toolchains']['base_path'])

    @property
    def all_releases(self) -> Dict[str, Dict[str, str]]:
        """Return all known releases in the data file."""

        try:
            return self._data['toolchains']['releases']
        except (KeyError, TypeError):
            return {}

    @property
    def releases(self) -> Dict[str, str]:
        """Return all releases for a the current platform."""

        # Successively match the distro and/or platform against available toolchains.
        # If none is found, use the default entry. If the default entry doesn't exist,
        # we want to know about it because that's a misconfiguration.
        platform_section: Optional[Dict[str, str]] = None

        if self._platform.distro_id in self.all_releases:
            platform_section = self.all_releases[self._platform.distro_id]
        elif self._platform.distro != ToolchainDistroName.DEFAULT:
            if str(self._platform) in self.all_releases:
                platform_section = self.all_releases[str(self._platform)]
            elif f"{self._platform.distro}.{self._platform.arch}" in self.all_releases:
                platform_section = self.all_releases[
                    f"{self._platform.distro}.{self._platform.arch}"]
            elif f"{self._platform.distro}.{self._platform.tag}" in self.all_releases:
                platform_section = self.all_releases[
                    f"{self._platform.distro}.{self._platform.tag}"]
            elif f"{self._platform.distro}" in self.all_releases:
                platform_section = self.all_releases[f"{self._platform.distro}"]

        if not platform_section:
            try:
                platform_section = self.all_releases['default']
            except KeyError:
                return {}

        return platform_section

    @property
    def versions(self) -> List[str]:
        """Return all known versions in the data file."""

        return self._data['toolchains']['versions']

    @property
    def aliases(self) -> Dict[str, str]:
        """Return all known version aliases in the data file."""

        return self._data['toolchains']['version_aliases']

    @property
    def revisions_dir(self) -> Optional[pathlib.Path]:
        """Return the legacy revisions directory for toolchain releases."""

        warnings.warn(("This is legacy toolchain usage. "
                       f"Call {self.__class__.__name__}.releases_dir() instead."),
                      DeprecationWarning, stacklevel=2)

        return self.base_path.joinpath('revisions')

    @property
    def releases_dir(self) -> pathlib.Path:
        """Return the directory where toolchain releases are installed."""

        return self.base_path.joinpath('releases')

    def search_releases(self, release_name: str) -> Optional[str]:
        """Search configured releases for a given release name."""

        try:
            return self.releases[release_name]
        except KeyError:
            return None


class Toolchain:
    """Represents the raw toolchain data."""

    def __init__(self, config: ToolchainConfig, release: str) -> None:
        """Construct a toolchain object from a supplied release and version."""

        self._config = config
        self._release = release

    @property
    def release(self) -> str:
        """Return the toolchain release ID."""

        return self._release

    @property
    def install_path(self) -> pathlib.Path:
        """Return the path to where the toolchain should be installed."""

        path = self._config.releases_dir / self.release

        # We need to determine if the configured release is a legacy release
        # that only appears in revisions_dir. We can tell the difference
        # because legacy releases are all identified by git commit hashes.
        if len(self.release) == 40 and set(self.release) <= set(string.hexdigits):
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                if self._config.revisions_dir:
                    path = self._config.revisions_dir / self.release

        return path

    def exec_path(self, version: Union[ToolchainVersionName, str]) -> Optional[pathlib.Path]:
        """Return a path to a specific toolchain version."""

        install_path = self.install_path

        if isinstance(version, ToolchainVersionName):
            version = version.value

        if version not in self._config.versions:
            try:
                version = self._config.aliases[version]
            except KeyError:
                raise ValueError(
                    f"Toolchain version `{version}' not defined in data file") from None

        return install_path / version


class Toolchains(Mapping[Union[ToolchainReleaseName, str], Toolchain]):
    """Represents a collection of toolchains that may or may not be installed on a system."""

    def __init__(self, config: ToolchainConfig) -> None:
        """Manipulate raw toolchain configuration data."""

        self._config = config
        self._available: Optional[List[str]] = None

    def _search_filesystem(self) -> List[str]:
        release_dirs: List[pathlib.Path] = []

        releases_dir: Optional[pathlib.Path] = self._config.releases_dir
        if releases_dir and releases_dir.exists():
            release_dirs.extend([path for path in releases_dir.iterdir() if path.is_dir()])

        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            revisions_dir: Optional[pathlib.Path] = self._config.revisions_dir

        if revisions_dir and revisions_dir.exists():
            release_dirs.extend([path for path in revisions_dir.iterdir() if path.is_dir()])

        if release_dirs:
            return [
                path.name for path in sorted(release_dirs, key=lambda path: path.stat().st_mtime,
                                             reverse=True)
            ]

        return []

    @property
    def available(self) -> List[str]:
        """Return a list of all installed toolchain releases ordered from newest to oldest."""

        if self._available is None:
            self._available = self._search_filesystem()

        return self._available

    @property
    def configured(self) -> List[str]:
        """Return a list of all configured toolchain releases."""

        configured: Set[Union[str, None]] = {
            self._config.search_releases(name.value)
            for name in ToolchainReleaseName
        }

        configured.add(self._config.search_releases('default'))

        return [release for release in configured if release is not None]

    @property
    def latest(self) -> Optional[str]:
        """Return the latest installed toolchain release.

        This can possibly be different from available[0]
        because we take into account the "latest" symlink present on end-user toolchain
        installations, which could potentially not be the "newest" for any reason. Therefore, these
        methods can be used to determine whether the "latest" symlink is out of date.
        """

        latest_symlink: Optional[pathlib.Path] = None
        try:
            latest_symlink = self._config.releases_dir.joinpath('latest')
        except AttributeError:
            latest_symlink = None

        if latest_symlink and latest_symlink.exists():
            return pathlib.Path(os.readlink(latest_symlink.absolute())).name

        return self.available[0] or None

    def __iter__(self) -> Iterator[str]:
        for release in set(self.available).union(set(self.configured)):
            yield release

    def __len__(self) -> int:
        return len(list(self.__iter__()))

    def __getitem__(self, key: Union[ToolchainReleaseName, str]) -> Toolchain:
        """Return the named toolchain release.

        This method supports two disjoint use cases:

          1. We don't know the release ID and pass a release name. Here, we are attempting to
             determine the release ID corresponding to the name for the current platform. If no
             such release name is configured, we raise an exception to indicate a potential
             misconfiguration or code error.

          2. We know the release ID and want to determine whether it is installed. In this case,
             None is returned if the release is not available to indicate it is not installed.
        """

        release: Optional[str] = None

        if isinstance(key, ToolchainReleaseName):
            # We don't know the release ID and are querying by name. This supports use case 1.
            if key == ToolchainReleaseName.LATEST:
                latest = self.latest
                if latest is None:
                    raise KeyError(key)

                release = self.latest

            release = self._config.search_releases(str(key))
        else:
            # We know the release ID and want to know whether it's installed. This supports use
            # case 2. This is the "short path" for the method because any string other than a
            # real release ID or release name causes a return.
            if key == str(ToolchainReleaseName.LATEST):
                release = self.latest
            if key in self.available:
                release = key
            if key in [name.value for name in ToolchainReleaseName]:
                release = self._config.search_releases(key)

        if release is None:
            raise KeyError(key)

        return Toolchain(self._config, release)


class _FormatterClass:
    """A protocol for a formatter class.

    HelpFormatter is missing this protocol in its inheritance tree in
    some versions of the mypy typeshed, which causes a spurious mypy error
    when trying to assign a custom HelpFormatter subclass. This is just
    here for NicerHelpFormatter to inherit from it and prevent the error.
    """

    def __call__(self, _: str) -> argparse.HelpFormatter:
        ...


# pylint: disable=protected-access
class NicerHelpFormatter(argparse.HelpFormatter, _FormatterClass):
    """A HelpFormatter with nicer output than the default."""

    def __init__(self, prog: str, indent_increment: int = 2, max_help_position: int = 32,
                 width=None) -> None:
        super().__init__(prog=prog, indent_increment=indent_increment,
                         max_help_position=max_help_position, width=width)

    def __call__(self, prog: str) -> argparse.HelpFormatter:
        return NicerHelpFormatter(prog)

    def _format_action(self, action: argparse.Action) -> str:
        result = super()._format_action(action)
        if isinstance(action, (argparse._SubParsersAction, DictChoiceAction)):
            return (" " * self._current_indent) + f"{result.lstrip()}"
        return result

    def _format_action_invocation(self, action: argparse.Action) -> str:
        if isinstance(action, (argparse._SubParsersAction, DictChoiceAction)):
            return ""
        if not action.option_strings:
            default = self._get_default_metavar_for_optional(action)
            metavar, = self._metavar_formatter(action, default)(1)
            return metavar

        parts: List[str] = []

        if action.nargs == 0:
            parts.extend(action.option_strings)
        else:
            default = self._get_default_metavar_for_optional(action)
            args_string = self._format_args(action, default)

            for option_string in action.option_strings:
                parts.append(option_string)

            return f"{' '.join(parts)} {args_string}"

        return ' '.join(parts)

    def _iter_indented_subactions(
            self, action: argparse.Action) -> Generator[argparse.Action, None, None]:
        if isinstance(action, (argparse._SubParsersAction, DictChoiceAction)):
            try:
                get_subactions = action._get_subactions
            except AttributeError:
                pass
            else:
                for subaction in get_subactions():
                    yield subaction
        else:
            for subaction in super()._iter_indented_subactions(action):
                yield subaction

    def _metavar_formatter(self, action: argparse.Action,
                           default_metavar: str) -> Callable[[int], Tuple[str, ...]]:
        if action.metavar is not None:
            result = action.metavar
        elif action.choices is not None:
            choice_strs = [f"{choice}" for choice in action.choices]
            result = f"{' | '.join(choice_strs)}"
            if action.required:
                result = f"({result})"
            else:
                result = f"[{result}]"
        else:
            result = default_metavar

        def _format(tuple_size: int) -> Tuple[str, ...]:
            if isinstance(result, tuple):
                return result
            return (result, ) * tuple_size

        return _format


# pylint: disable=protected-access, redefined-builtin, redefined-outer-name
class DictChoiceAction(argparse._StoreAction):
    """An action with nicer per-choice formatting."""

    class _ChoicesPseudoAction(argparse.Action):
        def __init__(self, name: str, aliases: List[str], help: Optional[str] = None) -> None:

            metavar = dest = name
            if aliases:
                metavar += f" {' | '.join(aliases)}"
                if self.required:
                    metavar = f"({metavar})"
                else:
                    metavar = f"[{metavar})"

            super().__init__(option_strings=[], dest=dest, help=help, metavar=metavar)

        def __call__(self, parser: argparse.ArgumentParser, namespace: argparse.Namespace,
                     values: Union[str, Sequence[Any], None],
                     option_string: Optional[str] = None) -> None:

            parser.print_help()
            parser.exit()

    def __init__(self, option_strings: List[str], dest: str, nargs: Optional[int] = None,
                 const: Optional[Any] = None, default: Optional[Any] = None,
                 type: Optional[type] = None, choices: Optional[Dict[str, str]] = None,
                 required: bool = False, help: Optional[str] = None,
                 metavar: Optional[str] = None) -> None:

        super().__init__(option_strings=option_strings, dest=dest, nargs=nargs, const=const,
                         default=default, type=type, choices=choices, required=required, help=help,
                         metavar=metavar)
        self.choices: Dict[str, str] = {}
        if choices:
            self.choices = choices

    def _get_subactions(self):
        choices_actions: List[argparse.Action] = []
        for name, help_text in self.choices.items():
            choice_action = self._ChoicesPseudoAction(name, [], help_text)
            choices_actions.append(choice_action)
        return choices_actions


class NicerArgumentParser(argparse.ArgumentParser):
    """An argument parser with nicer help output."""

    def __init__(self, prog: Optional[str] = None, usage: Optional[str] = None,
                 description: Optional[str] = None, epilog: Optional[str] = None,
                 parents: Optional[List[argparse.ArgumentParser]] = None, prefix_chars: str = '-',
                 fromfile_prefix_chars: Optional[str] = None,
                 argument_default: Optional[Any] = None, conflict_handler: str = 'error',
                 add_help: bool = True, allow_abbrev: bool = True) -> None:
        """Initialize a NicerParser."""

        super().__init__(prog=prog, usage=usage, description=description, epilog=epilog,
                         parents=parents if parents else [], formatter_class=NicerHelpFormatter,
                         prefix_chars=prefix_chars, fromfile_prefix_chars=fromfile_prefix_chars,
                         argument_default=argument_default, conflict_handler=conflict_handler,
                         add_help=add_help, allow_abbrev=allow_abbrev)
        self._optionals.title = 'Options'
        self._positionals.title = 'Queries'

    def format_help(self) -> str:
        formatter = self._get_formatter()
        formatter.add_text(self.description)
        formatter.add_usage(self.usage, self._actions, self._mutually_exclusive_groups,
                            prefix='Usage:\n  ')

        for action_group in self._action_groups:
            formatter.start_section(action_group.title)
            formatter.add_text(action_group.description)
            formatter.add_arguments(action_group._group_actions)
            formatter.end_section()

        formatter.add_text(self.epilog)

        return formatter.format_help()


if __name__ == '__main__':
    parser = NicerArgumentParser(
        description='Tool for querying information about mongodbtoolchain.', add_help=False)

    parser.add_argument('-h', '--help', action='help', default=argparse.SUPPRESS,
                        help='Show this help message and exit')
    parser.add_argument('-f', '--from-file', help='Specify a toolchain data file', metavar='FILE',
                        type=str, default=str(DEFAULT_DATA_FILE))
    parser.add_argument('-d', '--distro-id', help='Evergreen distro_id', type=str, required=True)
    parser.add_argument('-a', '--arch', help='Host architecture', type=str)

    subparsers = parser.add_subparsers(title='Commands', dest='command', required=True)

    show_parser = subparsers.add_parser(
        'show', description='Shows general toolchain collection info.', add_help=False,
        help='Show general toolchain collection info')
    config_parser = subparsers.add_parser('config',
                                          description='Shows toolchain configuration info.',
                                          add_help=False, help='Show toolchain configuration info')
    platform_parser = subparsers.add_parser('platform',
                                            description='Shows component parts of a distro_id.',
                                            add_help=False, help='Show parts of a distro_id')
    toolchain_parser = subparsers.add_parser('toolchain',
                                             description='Shows specific toolchain info.',
                                             add_help=False, help='Show specific toolchain info')

    show_parser.add_argument('-h', '--help', action='help', default=argparse.SUPPRESS,
                             help='Show this help message and exit')
    show_parser.add_argument(
        'query', action=DictChoiceAction, type=str, choices={
            'available': 'All installed toolchains',
            'configured': 'Toolchains configured for the distro_id',
            'latest': 'The most recent installed toolchain',
        })

    config_parser.add_argument('-h', '--help', action='help', default=argparse.SUPPRESS,
                               help='Show this help message and exit')
    config_parser.add_argument(
        'query', action=DictChoiceAction, type=str, choices={
            'base_path': 'Toolchain base execution path',
            'releases': 'All defined release names',
            'versions': 'All defined version names',
            'aliases': 'All defined aliases for version names',
        })

    platform_parser.add_argument('-h', '--help', action='help', default=argparse.SUPPRESS,
                                 help='Show this help message and exit')
    platform_parser.add_argument(
        'query', action=DictChoiceAction, type=str, choices={
            'distro': 'Show the "distro" component of the distro_id',
            'arch': 'Show the "arch" component of the distro_id',
            'tag': 'Show the information tag component of the distro_id',
        })

    toolchain_parser.add_argument('-h', '--help', action='help', default=argparse.SUPPRESS,
                                  help='Show this help message and exit')
    toolchain_parser.add_argument('-v', '--toolchain-version', help='Toolchain version', type=str,
                                  default=str(ToolchainVersionName.STABLE))
    toolchain_parser.add_argument('-r', '--release', help="Toolchain release", type=str,
                                  default=str(ToolchainReleaseName.CURRENT))
    toolchain_parser.add_argument(
        'query', action=DictChoiceAction, type=str, choices={
            'install_path': 'Toolchain installation path',
            'exec_path': 'Toolchain execution path',
        })

    parsed_args = parser.parse_args()
    obj: Optional[object] = None

    # Set up the objects required for each command
    toolchain_platform = ToolchainPlatform(distro_id=parsed_args.distro_id, arch=parsed_args.arch)
    if parsed_args.command == 'platform':
        obj = toolchain_platform
    elif parsed_args.command in ('show', 'config', 'toolchain'):
        try:
            toolchain_config = ToolchainConfig(
                pathlib.Path(parsed_args.from_file), platform=toolchain_platform)
        except ToolchainDataException as exc:
            print(exc, file=sys.stderr)
            sys.exit(1)

        toolchains = Toolchains(config=toolchain_config)

        if parsed_args.command == 'show':
            obj = toolchains
        elif parsed_args.command == 'config':
            obj = toolchain_config
        elif parsed_args.command == 'toolchain':
            obj = toolchains[parsed_args.release]
    else:
        print(f"Unknown command: {parsed_args.command}", file=sys.stderr)
        sys.exit(1)

    # Get and handle output
    output: Any
    attribute = getattr(obj, parsed_args.query)
    if callable(attribute):
        if attribute.__name__ == 'exec_path':
            output = attribute(parsed_args.toolchain_version)
        else:
            output = attribute()
    else:
        output = attribute  # If there is no output, it should indicate an error to the caller.

    # pylint: disable=invalid-name
    if output is not None:
        if isinstance(output, (tuple, list)):
            output = str.join(" ", output)
        elif isinstance(output, dict):
            output = '\n'.join([f"{k}: {v}" for k, v in output.items()])
        elif not isinstance(output, str):
            output = str(output)

    if output:
        print(output)
    else:
        sys.exit(1)
