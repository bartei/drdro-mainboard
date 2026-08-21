"""This software's own version — the single source of truth for the whole stack.

Under the one-version design the installed package version *is* the release version, and
the board's firmware is expected to report exactly the same string. Everything that needs
to know "what version are we" reads it from here so there is one place to mock in tests and
one place to change if the packaging ever moves.

Falls back to ``"unknown"`` when the package is not installed (a source checkout run
straight from the tree). ``unknown`` is unparseable by design — ``fw_compat`` treats that
as "cannot judge" and stays quiet rather than nagging a developer.
"""
import importlib.metadata

PACKAGE_NAME = "drdro-software"
UNKNOWN = "unknown"


def installed_version(package: str = PACKAGE_NAME) -> str:
    """The installed package version as ``vX.Y.Z``, or ``"unknown"``.

    Normalised with a leading ``v`` so it compares and displays identically to the board's
    firmware string, which is stamped from the git tag (``FW_VERSION=v1.2.0``).
    """
    try:
        raw = importlib.metadata.version(package)
    except importlib.metadata.PackageNotFoundError:
        return UNKNOWN
    raw = (raw or "").strip()
    if not raw:
        return UNKNOWN
    return raw if raw.startswith("v") else f"v{raw}"
