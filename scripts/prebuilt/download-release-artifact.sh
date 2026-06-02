#!/usr/bin/env bash
#
# Fetch one raw artifact from the rolling prebuilt release and decompress it in
# place. The rolling prebuilt manifest provides archive byte checksums for
# transport integrity. CI callers may pass the expected recipe key so release
# manifest drift between resolve and download fails before stamping.

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 <artifact> [expected-recipe-key]" >&2
    exit 2
fi

artifact=$1
expected_recipe_key=${2:-}
archive=${artifact}.bz2
recipe_entry=${artifact}.recipe-key

: "${PREBUILT_URL:?Need PREBUILT_URL pointing at the prebuilt artifact directory}"

tmpdir=$(mktemp -d)
cleanup_download_parts() {
    rm -rf "$tmpdir"
    rm -f "$archive.part" "$artifact.tmp"
}
trap cleanup_download_parts EXIT

manifest=$tmpdir/prebuilt.sha1

curl_progress=(--silent --show-error)
if [ -t 2 ]; then
    curl_progress=(--progress-bar)
fi

sha1_file() {
    local -a sha1_cmd

    if command -v sha1sum >/dev/null 2>&1; then
        sha1_cmd=(sha1sum)
    elif command -v shasum >/dev/null 2>&1; then
        sha1_cmd=(shasum -a 1)
    else
        echo "[!] Need sha1sum or shasum on PATH" >&2
        return 1
    fi

    "${sha1_cmd[@]}" "$1" | awk '{print $1}'
}

manifest_entry_value() {
    local entry=$1

    awk -v f="$entry" '$2 == f {print $1; found = 1; exit} END {exit !found}' "$manifest"
}

if ! curl --fail --retry 3 --retry-delay 1 --silent --show-error \
    -L -o "$manifest.part" "${PREBUILT_URL}/prebuilt.sha1"; then
    rm -f "$manifest.part"
    exit 1
fi
mv -f "$manifest.part" "$manifest"

if ! expected_archive_sha=$(manifest_entry_value "$archive"); then
    echo "[!] prebuilt manifest missing $archive" >&2
    exit 1
fi

if [ -n "$expected_recipe_key" ]; then
    if ! manifest_recipe_key=$(manifest_entry_value "$recipe_entry"); then
        echo "[!] prebuilt manifest missing $recipe_entry" >&2
        exit 1
    fi
    if [ "$manifest_recipe_key" != "$expected_recipe_key" ]; then
        echo "[!] prebuilt manifest recipe key mismatch for $artifact" >&2
        echo "[!] expected $expected_recipe_key" >&2
        echo "[!] actual   $manifest_recipe_key" >&2
        exit 1
    fi
fi

if ! curl --fail --retry 3 --retry-delay 1 "${curl_progress[@]}" \
    -L -o "$archive.part" "${PREBUILT_URL}/${archive}"; then
    rm -f "$archive.part"
    exit 1
fi

actual_archive_sha=$(sha1_file "$archive.part")
if [ "$actual_archive_sha" != "$expected_archive_sha" ]; then
    echo "[!] prebuilt archive checksum mismatch for $archive" >&2
    echo "[!] expected $expected_archive_sha" >&2
    echo "[!] actual   $actual_archive_sha" >&2
    exit 1
fi

mv -f "$archive.part" "$archive"
if ! bunzip2 -c "$archive" > "$artifact.tmp"; then
    rm -f "$artifact.tmp"
    exit 1
fi
mv -f "$artifact.tmp" "$artifact"
rm -f "$archive"
