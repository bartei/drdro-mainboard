"""Tests for GitHub release resolution, download and checksum verification.

The security-relevant behaviours pinned here:
  * asset lookup is EXACT — no fuzzy fallback that could match a foreign artifact
  * a release missing any required asset is skipped, not partially offered
  * verification fails CLOSED — a missing manifest entry is an error, not a skip
"""
import asyncio
import hashlib

import aiohttp
import pytest
from yarl import URL as YarlURL

URL_SENTINEL = YarlURL("https://dl/x")

from dro.comms.release import (
    CHECKSUM_ASSET,
    FIRMWARE_ASSET,
    GITHUB_REPO,
    SOFTWARE_ASSET,
    Release,
    ReleaseClient,
    ReleaseError,
    parse_checksums,
    parse_release,
    sha256_file,
    verify_file,
)


# ── fake aiohttp plumbing ────────────────────────────────────────────
class FakeContent:
    def __init__(self, data: bytes, chunk: int = 8):
        self._data = data
        self._chunk = chunk

    def iter_chunked(self, _n):
        async def gen():
            for i in range(0, len(self._data), self._chunk):
                yield self._data[i:i + self._chunk]
        return gen()


class FakeResponse:
    def __init__(self, *, json_data=None, text_data=None, body=b"", status=200,
                 headers=None, raise_exc=None):
        self._json = json_data
        self._text = text_data
        self.content = FakeContent(body)
        self.status = status
        self.headers = headers if headers is not None else {"Content-Length": str(len(body))}
        self._raise = raise_exc

    async def __aenter__(self):
        if self._raise:
            raise self._raise
        return self

    async def __aexit__(self, *a):
        return False

    def raise_for_status(self):
        if self.status >= 400:
            info = aiohttp.RequestInfo(
                url=URL_SENTINEL, method="GET",
                headers=aiohttp.typedefs.CIMultiDict(), real_url=URL_SENTINEL,
            )
            raise aiohttp.ClientResponseError(info, (), status=self.status)

    async def json(self):
        return self._json

    async def text(self):
        return self._text


class FakeSession:
    """Maps URL -> FakeResponse. Records every URL requested."""

    def __init__(self, routes: dict):
        self.routes = routes
        self.requested = []

    async def __aenter__(self):
        return self

    async def __aexit__(self, *a):
        return False

    def get(self, url, **kw):
        self.requested.append(url)
        try:
            return self.routes[url]
        except KeyError:                       # pragma: no cover — test wiring error
            raise AssertionError(f"unexpected URL {url}")


def factory(routes):
    session = FakeSession(routes)
    return (lambda: session), session


def _assets(*names):
    return [{"name": n, "browser_download_url": f"https://dl/{n}", "size": 10} for n in names]


def _payload(tag="v1.2.0", prerelease=False, names=None, name=None):
    return {
        "tag_name": tag,
        "name": name,
        "prerelease": prerelease,
        "assets": _assets(*(names if names is not None
                            else (SOFTWARE_ASSET, FIRMWARE_ASSET, CHECKSUM_ASSET))),
    }


# ── repo identity ────────────────────────────────────────────────────
def test_points_at_the_monorepo():
    # Regression guard: the standalone repos hold artifacts for a DIFFERENT board.
    assert GITHUB_REPO == "bartei/drdro-mainboard"


# ── parse_release ────────────────────────────────────────────────────
def test_parse_release_complete():
    rel = parse_release(_payload())
    assert rel.tag == "v1.2.0"
    assert rel.name == "v1.2.0"                     # falls back to the tag
    assert rel.prerelease is False
    assert rel.software_url.endswith(SOFTWARE_ASSET)
    assert rel.firmware_url.endswith(FIRMWARE_ASSET)
    assert rel.checksum_url.endswith(CHECKSUM_ASSET)
    assert rel.software_size == 10 and rel.firmware_size == 10


def test_parse_release_uses_release_name_when_present():
    assert parse_release(_payload(name="Shiny Release")).name == "Shiny Release"


@pytest.mark.parametrize("missing", [SOFTWARE_ASSET, FIRMWARE_ASSET, CHECKSUM_ASSET])
def test_parse_release_rejects_incomplete_asset_set(missing):
    names = [n for n in (SOFTWARE_ASSET, FIRMWARE_ASSET, CHECKSUM_ASSET) if n != missing]
    assert parse_release(_payload(names=names)) is None


def test_parse_release_no_fuzzy_asset_match():
    # A foreign artifact must NOT satisfy the firmware slot. This is the exact failure
    # mode the old endswith('.bin') fallback created.
    names = [SOFTWARE_ASSET, "drdro-app.bin", CHECKSUM_ASSET]
    assert parse_release(_payload(names=names)) is None


def test_parse_release_handles_missing_assets_key():
    assert parse_release({"tag_name": "v1.0.0"}) is None


def test_release_version_normalizes_the_tag():
    assert Release("1.2.0", "n", False, "a", "b", "c").version == "v1.2.0"
    assert Release("v1.2.0", "n", False, "a", "b", "c").version == "v1.2.0"


# ── parse_checksums ──────────────────────────────────────────────────
def test_parse_checksums_plain_and_binary_markers():
    text = (
        "aa11  drdro-software.zip\n"
        "bb22 *drdro-mainboard-app.bin\n"
    )
    sums = parse_checksums(text)
    assert sums == {"drdro-software.zip": "aa11", "drdro-mainboard-app.bin": "bb22"}


def test_parse_checksums_strips_dot_slash_prefix():
    # `sha256sum ./*` — the form tools/build-release.sh actually produces.
    assert parse_checksums("cc33  ./drdro-software.zip") == {"drdro-software.zip": "cc33"}


def test_parse_checksums_lowercases_digests():
    assert parse_checksums("AABB  x.bin") == {"x.bin": "aabb"}


def test_parse_checksums_skips_blanks_comments_and_malformed():
    text = "\n# a comment\ngarbage-no-name\ndd44  ok.bin\n   \n"
    assert parse_checksums(text) == {"ok.bin": "dd44"}


# ── hashing / verification ───────────────────────────────────────────
def test_sha256_file(tmp_path):
    p = tmp_path / "f.bin"
    p.write_bytes(b"hello world")
    assert sha256_file(p) == hashlib.sha256(b"hello world").hexdigest()


def test_sha256_file_streams_large_input(tmp_path):
    blob = b"x" * (256 * 1024)                  # exceeds the 64 KiB read chunk
    p = tmp_path / "big.bin"
    p.write_bytes(blob)
    assert sha256_file(p) == hashlib.sha256(blob).hexdigest()


def test_verify_file_accepts_matching_digest(tmp_path):
    p = tmp_path / "f.bin"
    p.write_bytes(b"data")
    verify_file(p, "f.bin", {"f.bin": hashlib.sha256(b"data").hexdigest()})


def test_verify_file_rejects_mismatch(tmp_path):
    p = tmp_path / "f.bin"
    p.write_bytes(b"data")
    with pytest.raises(ReleaseError, match="checksum mismatch"):
        verify_file(p, "f.bin", {"f.bin": "0" * 64})


def test_verify_file_fails_closed_on_missing_entry(tmp_path):
    # Unverifiable is not the same as verified — this must raise, never pass.
    p = tmp_path / "f.bin"
    p.write_bytes(b"data")
    with pytest.raises(ReleaseError, match="no entry"):
        verify_file(p, "f.bin", {})


# ── ReleaseClient ────────────────────────────────────────────────────
URL = "https://api/releases"


def test_list_releases_filters_prereleases_and_incomplete():
    payloads = [
        _payload(tag="v1.3.0-beta.1", prerelease=True),
        _payload(tag="v1.2.0"),
        _payload(tag="v1.1.0", names=[SOFTWARE_ASSET]),        # incomplete -> skipped
    ]
    f, _ = factory({URL: FakeResponse(json_data=payloads)})
    c = ReleaseClient(session_factory=f, releases_url=URL)

    stable = asyncio.run(c.list_releases())
    assert [r.tag for r in stable] == ["v1.2.0"]


def test_list_releases_includes_prereleases_when_asked():
    payloads = [_payload(tag="v1.3.0-beta.1", prerelease=True), _payload(tag="v1.2.0")]
    f, _ = factory({URL: FakeResponse(json_data=payloads)})
    c = ReleaseClient(session_factory=f, releases_url=URL)

    tags = [r.tag for r in asyncio.run(c.list_releases(include_prerelease=True))]
    assert tags == ["v1.3.0-beta.1", "v1.2.0"]


def test_list_releases_handles_empty_payload():
    f, _ = factory({URL: FakeResponse(json_data=None)})
    c = ReleaseClient(session_factory=f, releases_url=URL)
    assert asyncio.run(c.list_releases()) == []


def test_list_releases_wraps_network_errors():
    f, _ = factory({URL: FakeResponse(raise_exc=aiohttp.ClientError("boom"))})
    c = ReleaseClient(session_factory=f, releases_url=URL)
    with pytest.raises(ReleaseError, match="could not reach GitHub"):
        asyncio.run(c.list_releases())


def test_latest_returns_first_and_none_when_empty():
    f, _ = factory({URL: FakeResponse(json_data=[_payload(tag="v1.2.0")])})
    c = ReleaseClient(session_factory=f, releases_url=URL)
    assert asyncio.run(c.latest()).tag == "v1.2.0"

    f2, _ = factory({URL: FakeResponse(json_data=[])})
    c2 = ReleaseClient(session_factory=f2, releases_url=URL)
    assert asyncio.run(c2.latest()) is None


def test_fetch_checksums_parses_the_manifest():
    rel = parse_release(_payload())
    f, _ = factory({rel.checksum_url: FakeResponse(text_data="aa  drdro-software.zip")})
    c = ReleaseClient(session_factory=f)
    assert asyncio.run(c.fetch_checksums(rel)) == {"drdro-software.zip": "aa"}


def test_fetch_checksums_rejects_empty_manifest():
    rel = parse_release(_payload())
    f, _ = factory({rel.checksum_url: FakeResponse(text_data="")})
    c = ReleaseClient(session_factory=f)
    with pytest.raises(ReleaseError, match="empty or unparseable"):
        asyncio.run(c.fetch_checksums(rel))


def test_fetch_checksums_wraps_network_errors():
    rel = parse_release(_payload())
    f, _ = factory({rel.checksum_url: FakeResponse(raise_exc=aiohttp.ClientError("x"))})
    c = ReleaseClient(session_factory=f)
    with pytest.raises(ReleaseError, match="could not fetch"):
        asyncio.run(c.fetch_checksums(rel))


def test_download_writes_file_and_reports_progress(tmp_path):
    body = b"0123456789" * 3
    f, _ = factory({"https://dl/x": FakeResponse(body=body)})
    c = ReleaseClient(session_factory=f)
    seen = []
    dest = tmp_path / "x.bin"

    asyncio.run(c.download("https://dl/x", dest, on_progress=seen.append))

    assert dest.read_bytes() == body
    assert seen[-1] == 1.0
    assert all(0.0 <= v <= 1.0 for v in seen)
    assert seen == sorted(seen)                       # monotonic


def test_download_without_content_length_still_completes(tmp_path):
    f, _ = factory({"https://dl/x": FakeResponse(body=b"abc", headers={})})
    c = ReleaseClient(session_factory=f)
    seen = []
    dest = tmp_path / "x.bin"

    asyncio.run(c.download("https://dl/x", dest, on_progress=seen.append))

    assert dest.read_bytes() == b"abc"
    assert seen == [1.0]                              # only the terminal callback


def test_download_without_progress_callback(tmp_path):
    f, _ = factory({"https://dl/x": FakeResponse(body=b"abc")})
    c = ReleaseClient(session_factory=f)
    dest = tmp_path / "x.bin"
    assert asyncio.run(c.download("https://dl/x", dest)) == str(dest)


def test_download_wraps_network_errors(tmp_path):
    f, _ = factory({"https://dl/x": FakeResponse(raise_exc=aiohttp.ClientError("nope"))})
    c = ReleaseClient(session_factory=f)
    with pytest.raises(ReleaseError, match="download failed"):
        asyncio.run(c.download("https://dl/x", tmp_path / "x.bin"))


def test_download_http_error_becomes_release_error(tmp_path):
    # ClientResponseError is a ClientError subclass, so a 404 surfaces as ReleaseError
    # like any other download failure — callers only ever handle one exception type.
    f, _ = factory({"https://dl/x": FakeResponse(body=b"", status=404)})
    c = ReleaseClient(session_factory=f)
    with pytest.raises(ReleaseError, match="download failed"):
        asyncio.run(c.download("https://dl/x", tmp_path / "x.bin"))


def test_list_releases_http_error_becomes_release_error():
    f, _ = factory({URL: FakeResponse(json_data=None, status=500)})
    c = ReleaseClient(session_factory=f, releases_url=URL)
    with pytest.raises(ReleaseError, match="could not reach GitHub"):
        asyncio.run(c.list_releases())


def test_download_verified_happy_path(tmp_path):
    body = b"payload"
    f, _ = factory({"https://dl/x": FakeResponse(body=body)})
    c = ReleaseClient(session_factory=f)
    sums = {"x.bin": hashlib.sha256(body).hexdigest()}

    out = asyncio.run(c.download_verified("https://dl/x", tmp_path / "x.bin", "x.bin", sums))
    assert out == str(tmp_path / "x.bin")


def test_download_verified_rejects_corrupted_payload(tmp_path):
    f, _ = factory({"https://dl/x": FakeResponse(body=b"tampered")})
    c = ReleaseClient(session_factory=f)
    sums = {"x.bin": hashlib.sha256(b"expected").hexdigest()}

    with pytest.raises(ReleaseError, match="checksum mismatch"):
        asyncio.run(c.download_verified("https://dl/x", tmp_path / "x.bin", "x.bin", sums))
