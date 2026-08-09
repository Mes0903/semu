#!/usr/bin/env bash
#
# Internal prebuilt builder. It builds raw artifacts requested by a resolver plan
# and deliberately does not write .stamps/prebuilt-local stamps; the materializer commits
# stamps after the plan action succeeds.  Keeping this separate from the
# user-facing build-artifacts.sh avoids feeding local CLI side effects back
# into the CI materialization layer.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

# Reuse the build recipe library, but keep stamping outside this wrapper.  The
# materializer owns the commit point because only it knows which resolver action
# completed successfully.
. "$REPO_ROOT/scripts/prebuilt/artifact-recipes.sh"

build_prebuilt_artifact_classes "$@"
