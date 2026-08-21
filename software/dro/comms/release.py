"""GitHub release resolution, download and verification for the drDRO update system.

ONE VERSION FOR THE WHOLE STACK. Firmware and host software ship under a single monorepo
tag (see the root ``pyproject.toml`` and ``docs/update_system_todo.md``), so a "release"
here is always a *pair* of assets resolved from one GitHub release:

    drdro-software.zip          the software/ tree plus a prebuilt wheel
    drdro-mainboard-app.bin     the firmware application image

Deliberately Kivy-free and I/O-injectable: every network call goes through an
``aiohttp.ClientSession`` the caller may supply, so the whole module is exercisable
without a network or a UI.

Integrity: every asset is verified against the release's ``SHA256SUMS.txt`` before it is
used. Verification fails **closed** — a missing manifest entry is an error, not a skip,
because a silent skip is indistinguishable from a compromised or truncated download.
"""
from __future__ import annotations

import hashlib
import ssl
from dataclasses import dataclass

import aiohttp
import certifi

from kivy.logger import Logger

log = Logger.getChild(__name__)

# The monorepo. Firmware and software are released together from here; the standalone
# drdro-firmware-f4 / drdro-software-f4 repos are historical and must never be consulted —
# their artifacts are for a different board and a different version stream.
GITHUB_REPO = "bartei/drdro-mainboard"
RELEASES_URL = f"https://api.github.com/repos/{GITHUB_REPO}/releases"

# Asset names are a contract with tools/build-release.sh — keep them in sync.
SOFTWARE_ASSET = "drdro-software.zip"
FIRMWARE_ASSET = "drdro-mainboard-app.bin"
CHECKSUM_ASSET = "SHA256SUMS.txt"

# Verify TLS against certifi's bundle — the system CA store is unreliable on some hosts
# (notably NixOS, and minimal appliance images).
_SSL_CTX = ssl.create_default_context(cafile=certifi.where())

_CHUNK = 64 * 1024


class ReleaseError(Exception):
    """Release could not be resolved, downloaded or verified."""


@dataclass(frozen=True)
class Release:
    """One monorepo release: the tag plus the assets an update needs.

    ``software_url``/``firmware_url`` are absolute browser-download URLs. ``checksum_url``
    is the combined ``SHA256SUMS.txt`` covering both.
    """

    tag: str
    name: str
    prerelease: bool
    software_url: str
    firmware_url: str
    checksum_url: str
    software_size: int = 0
    firmware_size: int = 0

    @property
    def version(self) -> str:
        """The release version as the rest of the app spells it (``v1.2.3``)."""
        return self.tag if self.tag.startswith("v") else f"v{self.tag}"


def _asset(assets: list[dict], name: str) -> dict | None:
    """Exact-name asset lookup.

    Exact only, by design: a fuzzy fallback (``endswith('.bin')``) is precisely how an
    updater ends up flashing an artifact built for a different board.
    """
    return next((a for a in assets if a.get("name") == name), None)


def parse_release(payload: dict) -> Release | None:
    """Build a :class:`Release` from one GitHub API release object.

    Returns ``None`` when the release does not carry the full asset set — an incomplete
    release (a failed or partial CI run) must not be offered as an update target.
    """
    assets = payload.get("assets") or []
    sw = _asset(assets, SOFTWARE_ASSET)
    fw = _asset(assets, FIRMWARE_ASSET)
    sums = _asset(assets, CHECKSUM_ASSET)
    if not (sw and fw and sums):
        missing = [n for n, a in ((SOFTWARE_ASSET, sw), (FIRMWARE_ASSET, fw),
                                  (CHECKSUM_ASSET, sums)) if not a]
        log.debug("Skipping release %s — missing assets: %s",
                  payload.get("tag_name"), ", ".join(missing))
        return None
    tag = payload.get("tag_name") or ""
    return Release(
        tag=tag,
        name=payload.get("name") or tag,
        prerelease=bool(payload.get("prerelease")),
        software_url=sw["browser_download_url"],
        firmware_url=fw["browser_download_url"],
        checksum_url=sums["browser_download_url"],
        software_size=int(sw.get("size") or 0),
        firmware_size=int(fw.get("size") or 0),
    )


def parse_checksums(text: str) -> dict[str, str]:
    """Parse ``sha256sum`` output into ``{basename: hexdigest}``.

    Accepts both the binary (``*name``) and text (`` name``) markers, and the ``./name``
    form ``sha256sum ./*`` produces.
    """
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        # split(None, 1) on a non-blank line always yields a non-empty first field, so
        # the digest needs no emptiness guard here.
        digest, name = parts
        name = name.lstrip("*").strip()
        if name.startswith("./"):
            name = name[2:]
        out[name] = digest.lower()
    return out


def sha256_file(path) -> str:
    """Streaming SHA-256 of a file, hex-encoded."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(_CHUNK)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def verify_file(path, name: str, sums: dict[str, str]) -> None:
    """Check ``path`` against ``sums[name]``. Raises :class:`ReleaseError` on any doubt.

    Fails closed on a missing entry: an asset the manifest does not cover is unverifiable,
    which is not the same as verified.
    """
    want = sums.get(name)
    if want is None:
        raise ReleaseError(f"{name}: no entry in {CHECKSUM_ASSET} — cannot verify")
    got = sha256_file(path)
    if got != want:
        raise ReleaseError(f"{name}: checksum mismatch (expected {want}, got {got})")
    log.info("Verified %s (sha256 %s…)", name, want[:12])


class ReleaseClient:
    """Async client for resolving and fetching monorepo releases.

    ``session_factory`` exists so tests can inject a fake session; production passes
    nothing and gets a fresh :class:`aiohttp.ClientSession` per call.
    """

    def __init__(self, *, session_factory=None, releases_url: str = RELEASES_URL):
        self._session_factory = session_factory or aiohttp.ClientSession
        self._releases_url = releases_url

    async def _get_json(self, url: str):
        async with self._session_factory() as s:
            async with s.get(url, ssl=_SSL_CTX,
                             headers={"Accept": "application/vnd.github+json"}) as r:
                r.raise_for_status()
                return await r.json()

    async def _get_text(self, url: str) -> str:
        async with self._session_factory() as s:
            async with s.get(url, ssl=_SSL_CTX) as r:
                r.raise_for_status()
                return await r.text()

    async def list_releases(self, include_prerelease: bool = False) -> list[Release]:
        """All releases carrying a complete asset set, newest first (GitHub's order)."""
        try:
            data = await self._get_json(self._releases_url)
        except aiohttp.ClientError as e:
            raise ReleaseError(f"could not reach GitHub: {e}") from e
        out = []
        for payload in data or []:
            if payload.get("prerelease") and not include_prerelease:
                continue
            rel = parse_release(payload)
            if rel is not None:
                out.append(rel)
        return out

    async def latest(self, include_prerelease: bool = False) -> Release | None:
        """The newest complete release, or ``None`` when there is none."""
        rels = await self.list_releases(include_prerelease=include_prerelease)
        return rels[0] if rels else None

    async def fetch_checksums(self, release: Release) -> dict[str, str]:
        """Download and parse the release's combined ``SHA256SUMS.txt``."""
        try:
            text = await self._get_text(release.checksum_url)
        except aiohttp.ClientError as e:
            raise ReleaseError(f"could not fetch {CHECKSUM_ASSET}: {e}") from e
        sums = parse_checksums(text)
        if not sums:
            raise ReleaseError(f"{CHECKSUM_ASSET} is empty or unparseable")
        return sums

    async def download(self, url: str, dest, on_progress=None) -> str:
        """Stream ``url`` to ``dest``. Returns ``dest``. Progress is a 0..1 fraction."""
        dest = str(dest)
        try:
            async with self._session_factory() as s:
                async with s.get(url, ssl=_SSL_CTX) as r:
                    r.raise_for_status()
                    total = int(r.headers.get("Content-Length") or 0)
                    got = 0
                    with open(dest, "wb") as f:
                        async for chunk in r.content.iter_chunked(_CHUNK):
                            f.write(chunk)
                            got += len(chunk)
                            if on_progress and total:
                                on_progress(min(1.0, got / total))
        except aiohttp.ClientError as e:
            raise ReleaseError(f"download failed for {url}: {e}") from e
        if on_progress:
            on_progress(1.0)
        return dest

    async def download_verified(self, url: str, dest, name: str,
                                sums: dict[str, str], on_progress=None) -> str:
        """:meth:`download` then :func:`verify_file` — the only download path callers want."""
        await self.download(url, dest, on_progress=on_progress)
        verify_file(dest, name, sums)
        return str(dest)
