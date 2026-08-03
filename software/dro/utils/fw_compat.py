"""Firmware↔software version coherence.

ONE VERSION FOR THE WHOLE STACK. Firmware and host software are released together under a
single monorepo tag, so compatibility is not a range to negotiate — it is an equality to
enforce. The board's reported ``version`` must equal the installed software version; any
difference means the stack is mid-update or was updated out of band, and the UI asks the
user to realign the two.

This replaces the old ``COMPANION_FW_VERSION`` minimum-version gate, which belonged to the
era of independently versioned repos. There is no "minimum supported firmware" any more:
there is the version you are on, and there is wrong.

Unparseable board versions (``unknown``, a bare git hash from a build outside a tagged
checkout) are treated as "cannot judge" and stay quiet — a developer running a local build
should not be nagged, and a false alarm is worse than silence.
"""
import re

# Accepts releases ("v1.2.0"), prereleases ("v1.2.0-beta.1") and git-describe dev builds
# ("v1.2.0-3-g1234abc[-dirty]"); the describe suffix does not affect ordering.
_VERSION_RE = re.compile(
    r"^v?(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)"
    r"(?:-(?P<pre_token>[A-Za-z]+)\.(?P<pre_n>\d+))?"
)

# Relationship of the board's firmware to the installed software.
MATCH = "match"          # identical — the healthy state
BEHIND = "behind"        # board older than the software (the common update case)
AHEAD = "ahead"          # board newer — flashed out of band, software needs updating
UNKNOWN = "unknown"      # unparseable on either side — stay quiet


def parse_fw_version(version: str) -> tuple[int, int, int, int, int] | None:
    """Parse a version string into an orderable key, or None if unparseable.

    Key: (major, minor, patch, is_release, prerelease_n) — a prerelease sorts before the
    release it precedes (v1.2.0-beta.1 < v1.2.0). Unparseable strings ("unknown", bare git
    hashes from builds outside a tagged checkout) return None.
    """
    m = _VERSION_RE.match((version or "").strip())
    if not m:
        return None
    is_release = 0 if m["pre_token"] else 1
    return (
        int(m["major"]),
        int(m["minor"]),
        int(m["patch"]),
        is_release,
        int(m["pre_n"] or 0),
    )


def normalize_version(version: str) -> str:
    """Canonical display form: leading ``v``, describe/dirty suffix stripped.

    ``"1.2.0"`` and ``"v1.2.0-3-gabc1234-dirty"`` both render as ``"v1.2.0"``, so a dev
    build compares equal to the tag it was built from. Unparseable input is returned
    stripped but otherwise untouched, so the UI can still show whatever the board said.
    """
    raw = (version or "").strip()
    m = _VERSION_RE.match(raw)
    if not m:
        return raw
    core = f"v{m['major']}.{m['minor']}.{m['patch']}"
    if m["pre_token"]:
        core += f"-{m['pre_token']}.{m['pre_n']}"
    return core


def compare_versions(board_version: str, software_version: str) -> str:
    """Relationship of the board's firmware to the software: MATCH/BEHIND/AHEAD/UNKNOWN."""
    board = parse_fw_version(board_version)
    soft = parse_fw_version(software_version)
    if board is None or soft is None:
        return UNKNOWN
    if board == soft:
        return MATCH
    return BEHIND if board < soft else AHEAD


def versions_match(board_version: str, software_version: str) -> bool:
    """True only when both parse and are equal. Unknown is not a match."""
    return compare_versions(board_version, software_version) == MATCH


def fw_update_required(board_version: str, software_version: str) -> bool:
    """True when the stack is incoherent and the user should be asked to realign it.

    Both BEHIND and AHEAD qualify: under a single version, "newer firmware than software"
    is just as incoherent as the reverse, and is resolved the same way — bring the stack to
    one release. UNKNOWN never triggers (see the module docstring).
    """
    return compare_versions(board_version, software_version) in (BEHIND, AHEAD)


def mismatch_message(board_version: str, software_version: str) -> str:
    """Short, user-facing description of the mismatch. Empty when there is nothing to say."""
    rel = compare_versions(board_version, software_version)
    if rel in (MATCH, UNKNOWN):
        return ""
    board = normalize_version(board_version)
    soft = normalize_version(software_version)
    if rel == BEHIND:
        return f"Board firmware {board} is older than software {soft} — update required"
    return f"Board firmware {board} is newer than software {soft} — update required"
