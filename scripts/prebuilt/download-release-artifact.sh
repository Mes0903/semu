#!/usr/bin/env bash
#
# Fetch one raw artifact from the rolling prebuilt release and decompress it in
# place. This is transport only: recipe-key validation, stamping, and release
# policy belong to callers.

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <artifact>" >&2
    exit 2
fi

artifact=$1
archive=${artifact}.bz2

: "${PREBUILT_URL:?Need PREBUILT_URL pointing at the prebuilt artifact directory}"

curl_progress=(--silent --show-error)
if [ -t 2 ]; then
    curl_progress=(--progress-bar)
fi

if ! curl --fail --retry 3 --retry-delay 1 "${curl_progress[@]}" \
    -L -o "$archive.part" "${PREBUILT_URL}/${archive}"; then
    rm -f "$archive.part"
    exit 1
fi

mv -f "$archive.part" "$archive"
if ! bunzip2 -c "$archive" > "$artifact.tmp"; then
    rm -f "$artifact.tmp"
    exit 1
fi
mv -f "$artifact.tmp" "$artifact"
rm -f "$archive"
