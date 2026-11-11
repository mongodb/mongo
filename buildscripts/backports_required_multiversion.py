"""
A pydantic model, `BackportsRequired` for etc/backports_required_for_multiversion_tests.yml.

`BackportsRequired` has a `Section` for each old release's alias, like last-lts.
Each `Section` has a list of `Entry`s to apply to all suites, and optionally
a list of resmoke suites and suite specific `Entry`s.

    last-lts:
      all:
        - test_file: ...
          ticket: ...
"""

import re
from collections.abc import Iterator
from typing import Annotated, Any, Literal, get_args

import yaml
from pydantic import (
    AfterValidator,
    BaseModel,
    BeforeValidator,
    ConfigDict,
    Field,
    RootModel,
    ValidationError,
)
from pydantic_core import PydanticCustomError

BACKPORTS_FILE = "etc/backports_required_for_multiversion_tests.yml"
BACKPORT_REQUIRED_TAG = "backport_required_multiversion"
TICKET_RE = re.compile(r"[A-Z][A-Z0-9]*-[1-9][0-9]*")


def _check_ticket(ticket: str) -> str:
    if not TICKET_RE.fullmatch(ticket):
        raise PydanticCustomError(
            "ticket_format",
            "ticket '{ticket}' is not a Jira key like SERVER-12345",
            {"ticket": ticket},
        )
    return ticket


def _none_as(empty: type) -> BeforeValidator:
    """Accept YAML null (an empty `all:` or `suites:`) as an empty list or mapping."""
    return BeforeValidator(lambda value: empty() if value is None else value)


class Entry(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True, strict=True)
    test_file: str
    ticket: Annotated[str, AfterValidator(_check_ticket)]

    def __str__(self) -> str:
        return f"{self.test_file} ({self.ticket})"


def _check_no_duplicates(entries: list[Entry]) -> list[Entry]:
    seen, duplicates = set(), []
    for entry in entries:
        if entry in seen and entry not in duplicates:
            duplicates.append(entry)
        seen.add(entry)
    if duplicates:
        raise PydanticCustomError(
            "duplicate_entry",
            "listed more than once: {entries}",
            {"entries": ", ".join(str(entry) for entry in duplicates)},
        )
    return entries


EntryList = Annotated[list[Entry], _none_as(list), AfterValidator(_check_no_duplicates)]


class Section(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    all: EntryList = Field(description="Entries excluded from every multiversion suite.")
    suites: Annotated[dict[str, EntryList], _none_as(dict)] = Field(
        description="Entries excluded only from the suites whose tag matches the key."
    )


SectionName = Literal["last-lts", "last-continuous", "last-patch"]
SECTIONS: tuple[str, ...] = get_args(SectionName)


class BackportsRequired(RootModel[dict[SectionName, Section]]):
    """The whole file: a `Section` per old binary alias. Any of them may be omitted."""

    model_config = ConfigDict(strict=True)

    @classmethod
    def parse(cls, data: Any) -> "BackportsRequired":
        return cls.model_validate(data)

    @property
    def sections(self) -> dict[SectionName, Section]:
        return self.root

    def lists(self) -> Iterator[tuple[str, list[Entry]]]:
        """Yield each entry list with its location, like `last-lts.all` or
        `last-lts.suites.replica_sets_multiversion`.
        """
        for name, section in self.sections.items():
            yield f"{name}.all", section.all
            for suite, entries in section.suites.items():
                yield f"{name}.suites.{suite}", entries


def _expected_keys(parent: tuple) -> str:
    """Return the keys allowed in the mapping at `parent`, a section or an entry."""
    return "'all' and 'suites'" if len(parent) == 1 else "'test_file' and 'ticket'"


def _format_loc(loc: tuple) -> str:
    text = ""
    for part in loc:
        if isinstance(part, int):
            text += f"[{part}]"
        elif part != "[key]":
            text += f".{part}" if text else str(part)
    return text or "top level"


def _describe_errors(err: ValidationError) -> list[str]:
    """Turns a ValidationError into one readable message per problem."""
    messages = []
    for error in err.errors():
        loc = tuple(error["loc"])
        if error["type"] == "literal_error" and loc[1:] == ("[key]",):
            messages.append(f"Unknown section '{loc[0]}'. Expected {', '.join(SECTIONS)}.")
        elif error["type"] == "extra_forbidden":
            messages.append(
                f"{_format_loc(loc[:-1])}: Unknown key '{loc[-1]}'. "
                f"Expected {_expected_keys(loc[:-1])}."
            )
        elif error["type"] == "missing":
            hint = (
                " (use an empty value if it has no entries)" if loc[-1] in ("all", "suites") else ""
            )
            messages.append(f"{_format_loc(loc[:-1])}: missing key '{loc[-1]}'{hint}.")
        elif not loc:
            messages.append(f"The top level must be a mapping of {', '.join(SECTIONS)}.")
        else:
            messages.append(f"{_format_loc(loc)}: {error['msg']}.")
    return messages


class InvalidBackportsFile(ValueError):
    """The file is not valid YAML or does not match the model."""

    def __init__(self, errors: list[str]):
        super().__init__("\n".join(errors))
        self.errors = errors


def parse_text(text: str) -> BackportsRequired:
    """Parse the file's contents. Raises InvalidBackportsFile."""
    try:
        return BackportsRequired.parse(yaml.safe_load(text))
    except yaml.YAMLError as e:
        raise InvalidBackportsFile([f"Not valid YAML:\n{e}"]) from e
    except ValidationError as e:
        raise InvalidBackportsFile(_describe_errors(e)) from e
