"""Tests for the single source of truth on "what version are we"."""
import importlib.metadata

import pytest

from dro.utils import version as version_mod
from dro.utils.version import UNKNOWN, installed_version


def test_installed_version_adds_v_prefix(monkeypatch):
    monkeypatch.setattr(importlib.metadata, "version", lambda pkg: "1.2.0")
    assert installed_version() == "v1.2.0"


def test_installed_version_keeps_existing_v_prefix(monkeypatch):
    monkeypatch.setattr(importlib.metadata, "version", lambda pkg: "v1.2.0")
    assert installed_version() == "v1.2.0"


def test_installed_version_strips_whitespace(monkeypatch):
    monkeypatch.setattr(importlib.metadata, "version", lambda pkg: "  1.2.0 ")
    assert installed_version() == "v1.2.0"


def test_missing_package_is_unknown(monkeypatch):
    def boom(pkg):
        raise importlib.metadata.PackageNotFoundError(pkg)

    monkeypatch.setattr(importlib.metadata, "version", boom)
    assert installed_version() == UNKNOWN


def test_empty_metadata_is_unknown(monkeypatch):
    monkeypatch.setattr(importlib.metadata, "version", lambda pkg: "")
    assert installed_version() == UNKNOWN


def test_unknown_is_unparseable_so_it_never_nags():
    # The contract between this module and fw_compat: "unknown" must not parse, so a
    # source checkout is treated as "cannot judge" rather than as a mismatch.
    from dro.utils.fw_compat import fw_update_required, parse_fw_version

    assert parse_fw_version(UNKNOWN) is None
    assert fw_update_required("v1.2.0", UNKNOWN) is False


def test_package_name_is_the_distribution_name():
    assert version_mod.PACKAGE_NAME == "drdro-software"


@pytest.mark.parametrize("raw,expected", [
    ("1.2.0", "v1.2.0"),
    ("1.2.0b1", "v1.2.0b1"),          # PEP 440 spelling passes through untouched
    ("v2.0.0-beta.1", "v2.0.0-beta.1"),
])
def test_various_metadata_spellings(monkeypatch, raw, expected):
    monkeypatch.setattr(importlib.metadata, "version", lambda pkg: raw)
    assert installed_version() == expected
