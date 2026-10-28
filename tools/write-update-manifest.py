"""Generate the public update fallback from published release metadata only."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import urllib.request

REPOSITORY = "littletaro97-arch/honor-magic-visual-community"
MAX_BYTES = 512 * 1024
VERSION = re.compile(r"[vV]?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?\Z")


def version_key(tag):
    if not isinstance(tag, str) or len(tag) > 80:
        return None
    match = VERSION.fullmatch(tag)
    if not match:
        return None
    pre, build = match[4], match[5]
    for field in (pre, build):
        if field is not None and any(not item for item in field.split(".")):
            return None
    if pre and any(item.isdigit() and len(item) > 1 and item[0] == "0" for item in pre.split(".")):
        return None
    parts = tuple((0, int(item)) if item.isdigit() else (1, item) for item in pre.split(".")) if pre else ()
    return tuple(int(match[i]) for i in (1, 2, 3)), not bool(pre), parts


def generate(releases):
    if not isinstance(releases, list) or len(releases) > 500:
        raise ValueError("Release list is invalid or incomplete")
    selected = {}
    for release in releases:
        if not isinstance(release, dict) or type(release.get("draft")) is not bool:
            raise ValueError("Release flags missing")
        if release["draft"]:
            continue
        key = version_key(release.get("tag_name"))
        if key is None:
            continue
        if type(release.get("prerelease")) is not bool:
            raise ValueError("Release channel missing")
        published = release.get("published_at")
        if not isinstance(published, str) or not published or dt.datetime.fromisoformat(published.replace("Z", "+00:00")).utcoffset() is None:
            raise ValueError("Only published releases may enter the manifest")
        preview = release["prerelease"] or not key[1]
        if preview in selected and key <= selected[preview][0]:
            continue
        notes = release.get("body") or ""
        if not isinstance(notes, str):
            raise ValueError("Release notes invalid")
        selected[preview] = (key, dict(tag_name=release["tag_name"], draft=False, prerelease=preview,
                                      published_at=published, body=notes[:16000]))
    # Retain the newest stable and preview independently, even after many previews.
    return dict(schemaVersion=1, repository=REPOSITORY,
                generatedAtUtc=dt.datetime.now(dt.timezone.utc).isoformat(),
                releases=[value[1] for _, value in sorted(selected.items())])


def fetch():
    headers = {"User-Agent": "MagicCommunity-ReleaseManifest", "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2026-03-10"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    releases = []
    for page in range(1, 6):
        request = urllib.request.Request(f"https://api.github.com/repos/{REPOSITORY}/releases?per_page=100&page={page}", headers=headers)
        with urllib.request.urlopen(request, timeout=12) as response:
            data = response.read(2 * 1024 * 1024 + 1)
        if len(data) > 2 * 1024 * 1024:
            raise ValueError("Release response exceeds limit")
        rows = json.loads(data)
        if not isinstance(rows, list) or len(rows) > 100:
            raise ValueError("Release response invalid")
        releases.extend(rows)
        if len(rows) < 100:
            return releases
    raise ValueError("Release pagination incomplete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", type=Path)
    source.add_argument("--fetch", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    releases = fetch() if args.fetch else json.loads(args.input.read_text(encoding="utf-8-sig"))
    result = generate(releases)
    content = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if len(content.encode("utf-8")) > MAX_BYTES:
        raise ValueError("Manifest exceeds client limit")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="utf-8")
    print(f"Manifest generated: {len(result['releases'])} published channels")


if __name__ == "__main__":
    main()
