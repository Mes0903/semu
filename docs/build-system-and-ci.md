# semu Build System and CI/CD Pipeline

## Big Picture

This document explains the build system and CI/CD pipeline in semu. The overall flow can be read as four related contexts: local development, CI artifact preparation, test consumption, and publishing.

The main difference between those contexts is who decides whether to use a release artifact or rebuild from source. The local path stays close to the original Make file-target model, where the developer explicitly decides when to rebuild from source. CI automatically compares the current checkout, the rolling release, and the workflow-local cache so that guest artifacts do not need to be rebuilt from scratch on every push.

For local builds, Make treats `Image`, `rootfs.cpio`, and `test-tools.img` as regular file targets. When a Make target depends on one of those files, or when the user explicitly requests one of those filenames, Make downloads the corresponding `.bz2` from the rolling `prebuilt` release if the raw artifact is missing.

By default, `make check` requires `Image` and `rootfs.cpio`, then derives the default `ext4.img` from `rootfs.cpio`. `test-tools.img` is obtained through `make test-tools.img` or through the source-build `test-tools` class.

When a developer wants to rebuild from the current working tree, they explicitly run `make build-artifacts`, letting the build system decide which parts need to be rebuilt. Source builds use separate Buildroot output directories for the default rootfs and the test-tools rootfs, so files added for test-tools do not affect the default `rootfs.cpio` or `ext4.img`.

```text
local developer
  │
  └─ make / make check
     │
     └─ mk/external.mk
        │
        ├─ artifact exists        -> use existing local file
        ├─ artifact missing       -> download and verify release .bz2
        └─ explicit source build  -> make build-artifacts
```

In CI, `guest-artifact-build` first determines the guest artifact set required by the current checkout, then compares it with the rolling release. If the release already matches the current requirements, this job skips materialization, packaging, and upload, and later test jobs download artifacts directly from the release.

If the release does not match the current requirements, CI restores the workflow-local raw cache, then checks whether the restored cache contains usable artifacts. When neither the release nor the workflow-local raw cache matches the current requirements, CI builds from source. The workflow-local raw cache is CI's main acceleration layer.

```text
Actions
  │
  └─ guest-artifact-build
     │
     ├─ resolve current checkout against release manifest
     │
     ├─ release current?
     │  │
     │  ├─ yes -> skip materialize/package/upload in this job
     │  └─ no  -> restore workflow-local raw cache
     │
     ├─ resolve again after cache restore
     │
     ├─ materialize supplied plan
     │  │
     │  ├─ use-action-cache restored raw artifacts
     │  ├─ download matching release artifacts
     │  └─ build changed artifacts from source
     │
     └─ package and upload artifacts for later jobs
```

The test and publish stages reuse the artifact content decided earlier. Test jobs obtain artifacts through `.github/actions/provide-guest-artifacts`, then run `make`, `make check`, or `.ci/autorun.sh`. When the release is determined to need an update in the current workflow run, test jobs use the raw artifacts produced by that workflow run.

On a master push, if artifacts need an update and all required tests pass, `publish-prebuilt` verifies the packaged artifacts and updates the rolling `prebuilt` release.

```text
Test jobs
  │
  ├─ provide-guest-artifacts
  │  │
  │  ├─ release stale   -> use this workflow run raw bundle
  │  └─ release current -> download release artifacts
  │
  └─ make / make check / .ci/autorun.sh

publish-prebuilt
  │
  ├─ master push + release_needs_update + all required tests passed
  ├─ verify packaged bundle
  └─ update rolling prebuilt release
```

## File Responsibilities

The files below are ordered roughly by the main call flow. Within the same layer, helpers that are sourced are listed after their callers. Local source builds and CI builds share the recipes under `scripts/prebuilt/`.

### Local Source Build Path

The local source-build path starts from a Make target, then enters the user CLI and the shared recipes.

- `Makefile`: provides local entry points such as `build-artifacts`, `check`, and `bench-login`, and includes `mk/external.mk`.
- `mk/external.mk`: defines local raw artifact download targets for `Image`, `rootfs.cpio`, and `test-tools.img`.
- `scripts/build-artifacts.sh`: user CLI called by `make build-artifacts`; it parses build classes, local flags, Buildroot cleanup policy, and the local `ext4.img` derived output.
- `scripts/prebuilt/artifact-classes.sh`: shared source-build artifact contract; it defines class names and raw output filenames.
- `scripts/prebuilt/artifact-recipes.sh`: shared source-build recipe library; it actually builds the Linux, Buildroot rootfs, and test-tools raw artifacts.
- `scripts/prebuilt/artifact-recipe.env`: external revisions and size constants loaded by `artifact-recipes.sh`.
- `scripts/prebuilt/test-tools-recipe.sh`: test-tools recipe selection logic shared by `artifact-recipes.sh` and the CI recipe key.
- `scripts/prebuilt/download-release-artifact.sh`: release transport helper shared by local Make downloads and the CI materializer.

### CI Prebuilt Layer

The CI prebuilt layer starts from the workflow, decides artifact sources, then materializes, stamps, and packages artifacts.

- `.github/workflows/main.yml`: starts `guest-artifact-build` and connects cache, workflow artifacts, the publish gate, and job outputs.
- `.ci/prebuilt/artifact-inputs.sh`: sourced by the resolver, materializer, and package helpers; it connects the shared artifact contract to CI recipe-key helpers and plan-key helpers.
- `.ci/prebuilt/resolve-artifacts.sh`: produces a materialization plan from the current checkout, the release manifest, and the restored raw cache.
- `.ci/prebuilt/materialize-artifacts.sh`: executes the plan produced by the resolver; the action may be `use-action-cache`, `download-release`, or `build`.
- `.ci/prebuilt/build-plan-artifacts.sh`: internal CI build wrapper called by the materializer when source builds are needed; it sources `scripts/prebuilt/artifact-recipes.sh`.
- `.ci/prebuilt/stamp-artifacts.sh`: writes `.stamps/prebuilt-local/*.recipe-key` stamps after successful materialization.
- `.ci/prebuilt/package.sh`: compresses raw artifacts and writes the release manifest.
- `.ci/prebuilt/verify-package.sh`: checks packaged artifacts and manifest shape before publish.

### Test And GitHub Helpers

After the artifact decision is complete, test jobs obtain the environment and guest artifacts through composite actions. GitHub helpers provide common workflow-output and PR-suggestion operations.

- `.github/actions/setup-semu/action.yml`: GitHub test dependency installation.
- `.github/actions/provide-guest-artifacts/action.yml`: composite action used by test jobs to obtain guest artifacts.
- `.ci/github/append-github-output.sh`: validated `$GITHUB_OUTPUT` writer.
- `.ci/github/suggest-format.sh`: reviewdog/GitHub PR formatting suggestions.

## Local Build Path

The local flow starts from Make file targets. When a developer runs `make`, the build produces the emulator binary and `minimal.dtb`. When they run `make check`, Make needs `Image`, `rootfs.cpio`, and, in the default external-rootfs mode, `ext4.img`. Make checks whether those files already exist. Existing files are reused; missing files are downloaded according to the rules in `mk/external.mk` from the rolling `prebuilt` release.

The Makefile includes `mk/external.mk` before the artifact-consuming targets, making guest artifact names such as `KERNEL_DATA`, `INITRD_DATA`, and `TEST_TOOLS_DATA` regular file targets. `check` connects `KERNEL_DATA`, the boot-mode-dependent `INITRD_DEP`, and `DISKIMG_FILE` to the emulator launch arguments. The relationship can be simplified as follows:

```make
...
include mk/external.mk

...

check: $(BIN) minimal.dtb $(KERNEL_DATA) $(INITRD_DEP) $(DISKIMG_FILE) $(SHARED_DIRECTORY)
	@$(call notice, Ready to launch Linux kernel. Please be patient.)
	$(Q)./$(BIN) $(strip $(KERNEL_OPT) $(DTB_OPT) $(HEADLESS_OPT) \
	    $(INITRD_OPT) $(DISKIMG_OPT) $(VIRTIOFS_OPT) \
	    $(SMP_OPT) $(NETDEV_OPT))
```

The download rule in `mk/external.mk` is intentionally thin and only downloads missing files. After the download completes, the helper decompresses the archive into the raw artifact and removes the `.bz2`, so the workspace usually contains raw artifacts rather than compressed archives.

This local download path verifies the release archive checksum against `prebuilt.sha1`, ensuring that the downloaded `.bz2` bytes match the release manifest before decompression. Local `make check` uses existing or downloaded raw artifacts. Therefore, when a developer changes a guest config or build recipe, they explicitly run `make build-artifacts` to test artifacts produced from the current source.

```make
define download_prebuilt_artifact
$(1):
	$$(Q)printf '  GET\t$$@\n'
	$$(Q)PREBUILT_URL="$$(PREBUILT_URL)" scripts/prebuilt/download-release-artifact.sh "$$@"
endef

$(foreach artifact,$(PREBUILT_ARTIFACTS),$(eval $(call download_prebuilt_artifact,$(artifact))))
```

`ext4.img` is produced differently. When the default external rootfs is enabled, `ext4.img` is a local derived file generated from `rootfs.cpio`. A fresh `make check` usually ensures that `Image` and `rootfs.cpio` exist first. If `ext4.img` is missing, it then runs `scripts/rootfs_ext4.sh rootfs.cpio ext4.img`. The rootfs helper exposes tool hooks so CI or developers can provide a coherent fakeroot-compatible toolchain without adding platform policy to the helper itself.

```make
ext4.img: $(INITRD_DATA) scripts/rootfs_ext4.sh
	$(Q)MKFS_EXT4="$(MKFS_EXT4)" ROOTFS_CPIO="$(ROOTFS_CPIO)" \
	    ROOTFS_CP="$(ROOTFS_CP)" ROOTFS_CHOWN="$(ROOTFS_CHOWN)" \
	    ROOTFS_FAKEROOT_SHELL="$(ROOTFS_FAKEROOT_SHELL)" \
	    scripts/rootfs_ext4.sh $(INITRD_DATA) $@
```

To regenerate guest artifacts from the current working tree, developers use `make build-artifacts`. The Makefile is a thin wrapper here; CLI parsing lives in `scripts/build-artifacts.sh`.

```make
ARTIFACTS ?= all
.PHONY: build-artifacts
build-artifacts:
	scripts/build-artifacts.sh $(ARTIFACTS)
```

`build-artifacts.sh` first maps the user-supplied class to three internal boolean values. `image` builds the Linux kernel artifact, `rootfs` builds `rootfs.cpio` and the default `ext4.img`, `test-tools` builds the optional disk, and `all` enables all three classes. `--x11` and `--directfb2-test` are test-tools recipe entries. When the user provides a recipe flag explicitly, the script builds the selected entry.

```bash
...
case "$1" in
    image)      BUILD_LINUX=1 ;;
    rootfs)     BUILD_ROOTFS=1 ;;
    test-tools) BUILD_TEST_TOOLS=1 ;;
    all)
        BUILD_LINUX=1
        BUILD_ROOTFS=1
        BUILD_TEST_TOOLS=1
        ;;
    --x11)            add_test_tools_recipe_entry x11 ;;
    --directfb2-test) add_test_tools_recipe_entry directfb2 ;;
esac
...
```

Before the actual build, `--clean-build` and `--full-rebuild` process old outputs. Both options remove the selected raw artifact outputs so that a failed source build does not leave stale `Image`, `rootfs.cpio`, `ext4.img`, or `test-tools.img` in the workspace.

`--clean-build` removes Buildroot output directories while preserving the `buildroot/` checkout and download cache. `--full-rebuild` removes external source/build trees so the next step reclones the pinned revisions.

```bash
...
if [[ $CLEAN_BUILD -eq 1 || $FULL_REBUILD -eq 1 ]]; then
    remove_selected_artifact_outputs
fi

if [[ $FULL_REBUILD -eq 1 ]]; then
    ...
    remove_existing_paths "Buildroot source tree for full rebuild" buildroot
    remove_existing_paths "Linux source tree for full rebuild" linux
elif [[ $CLEAN_BUILD -eq 1 ]]; then
    remove_existing_paths "default Buildroot output for clean build" \
        "$BUILDROOT_DEFAULT_OUTPUT" buildroot/output
    remove_existing_paths "test-tools Buildroot output for clean build" \
        "$BUILDROOT_TEST_TOOLS_OUTPUT"
fi
...
```

The actual build logic is centralized in `scripts/prebuilt/artifact-recipes.sh`. This file is used by both the local CLI and the CI internal builder. It connects source trees, Buildroot outputs, and final raw artifacts. The default rootfs and test-tools rootfs use separate Buildroot outputs so that test-tools recipe changes remain isolated from the default rootfs.

```bash
BUILDROOT_DEFAULT_OUTPUT=${BUILDROOT_DEFAULT_OUTPUT:-buildroot/output-default}
BUILDROOT_TEST_TOOLS_OUTPUT=${BUILDROOT_TEST_TOOLS_OUTPUT:-buildroot/output-test-tools}

buildroot_make() {
    local output_dir=$1
    shift

    ASSERT mkdir -p "$output_dir"
    ASSERT make -C buildroot O="$PWD/$output_dir" "$@"
}
```

The `image`, `rootfs`, and `test-tools` recipe entrypoints all run under `with_buildroot_lock` because they share the same `buildroot/` checkout, configuration, host toolchain, and output directories. The lock uses an atomic symlink at `.semu-buildroot.lock` by default, or at `BUILDROOT_LOCK_PATH` when that variable is set. Creating the symlink both acquires the lock and publishes the owner token, so waiters never observe a successfully acquired lock before its owner is recorded. After `ln -s` succeeds, the recipe verifies that the configured lock path itself points at the current owner token, which also catches stale directory locks left by older versions.

```bash
BUILDROOT_LOCK_PATH=${BUILDROOT_LOCK_PATH:-.semu-buildroot.lock}

buildroot_acquire_lock() {
    ...
    BUILDROOT_LOCK_TOKEN=$(buildroot_new_lock_token)
    while ! ln -s "$BUILDROOT_LOCK_TOKEN" "$BUILDROOT_LOCK_PATH" 2>/dev/null; do
        ...
    done
    ...
}
```

The recipe does not automatically remove stale source-build locks. A stale lock should only be left behind when an artifact source build is interrupted after acquiring the lock, such as when the user kills the build or a CI runner is terminated. Portable shell/filesystem operations do not provide an atomic "remove this lock only if it still has this owner token" operation, so a read/compare/remove cleanup can race with another process that already acquired a fresh lock. When a stale or unrecognized lock is reported, first confirm that no semu artifact source build is still running, then remove the reported lock path manually. The default reported path is `.semu-buildroot.lock`, and `make distclean` also removes that default path.

When building the default rootfs, the shared recipe uses the base Buildroot config to produce `buildroot/output-default/images/rootfs.cpio`, then publishes it as `rootfs.cpio` in the repository root. If the local CLI does not receive `--no-ext4`, it also derives `ext4.img` after `rootfs.cpio` is produced.

```bash
do_rootfs_unlocked() {
    build_buildroot_rootfs default "$BUILDROOT_DEFAULT_OUTPUT"

    echo "Publishing rootfs.cpio"
    ASSERT cp -f "$BUILDROOT_DEFAULT_OUTPUT/images/rootfs.cpio" ./rootfs.cpio
}

derive_local_ext4_from_rootfs() {
    if [[ $NO_EXT4 -eq 1 ]]; then
        echo "Skipping ext4.img build (--no-ext4)"
        return
    fi

    ASSERT ./scripts/rootfs_ext4.sh ./rootfs.cpio ./ext4.img
}
```

When building test-tools, the recipe uses `buildroot/output-test-tools`. If the recipe includes `x11`, the Buildroot config is layered with `configs/x11.config`, and the C++ runtime needed at runtime is added to the `test-tools.img` overlay.

If the recipe includes `directfb2`, `do_extra_packages` first installs DirectFB2 and DirectFB examples into `directfb/`, copies them into `extra_packages/`, and finally overlays `extra_packages/` into `test-tools.img`. X11 and DirectFB2 payloads enter the test-tools output, while the default `rootfs.cpio` and `ext4.img` keep using the default rootfs recipe.

```bash
do_test_tools_unlocked() {
    local buildroot_mode
    local buildroot_output=$BUILDROOT_TEST_TOOLS_OUTPUT
    local test_tools_rootfs

    configure_test_tools_recipe_flags

    if [[ $BUILD_X11 -eq 1 ]]; then
        buildroot_mode=x11
    else
        buildroot_mode=default
    fi
    build_buildroot_rootfs "$buildroot_mode" "$buildroot_output"
    test_tools_rootfs=./$buildroot_output/images/rootfs.cpio

    if [[ $BUILD_DIRECTFB_TEST -eq 1 ]]; then
        do_extra_packages "$buildroot_output"
        if [[ $BUILD_X11 -eq 1 ]]; then
            stage_cxx_runtime "$buildroot_output"
        fi
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    elif [[ $BUILD_X11 -eq 1 ]]; then
        ASSERT rm -rf extra_packages
        stage_cxx_runtime "$buildroot_output"
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    else
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB"
    fi
}
```

## CI Artifact Contract

The contract between CI and the rolling release defines fixed raw artifact filenames and a sha1sum-format manifest. The release contains `Image.bz2`, `rootfs.cpio.bz2`, `test-tools.img.bz2`, and `prebuilt.sha1`. `prebuilt.sha1` records both the byte checksum of each `.bz2` archive and the recipe key for each artifact class.

The class registry for this contract is defined by `scripts/prebuilt/artifact-classes.sh`, because class names and raw output filenames belong to the source-build contract. CI policy lives under `.ci/prebuilt/`.

Each registry row contains a class name and the raw artifact filename for that class.

`.ci/prebuilt/artifact-inputs.sh` sources this contract, then adds CI recipe-key helpers and plan-key helpers.

Shell-safe names used by plan variables are derived from class names. For example, `test-tools` becomes `test_tools`.

```bash
source_artifact_class_registry() {
    printf '%s\t%s\n' image Image
    printf '%s\t%s\n' rootfs rootfs.cpio
    printf '%s\t%s\n' test-tools test-tools.img
}
```

A recipe key has two kinds of inputs. The first kind is real files, such as the Buildroot config, Linux config, BusyBox config, `target/init`, `scripts/rootfs_ext4.sh`, and test-tools-specific files such as `configs/x11.config` or `configs/meson-riscv-cross-file`. The second kind is recipe variables that do not directly appear as files but affect the output content, such as `BUILDROOT_REV`, `LINUX_REV`, `TEST_TOOLS_SIZE_MB`, and the normalized `PREBUILT_TEST_TOOLS_RECIPE` selection.

`prebuilt_class_recipe_manifest` expands both groups into a sha1sum-style manifest. `prebuilt_class_recipe_key` then folds that full manifest into a single SHA-1. Every later CI decision about whether an artifact matches the current checkout uses this class recipe key.

```bash
prebuilt_class_recipe_manifest() {
    local class=$1
    local -a inputs=()
    local input

    while IFS= read -r input; do
        inputs+=("$input")
    done < <(prebuilt_class_inputs "$class")

    if [ "${#inputs[@]}" -eq 0 ]; then
        echo "[!] Prebuilt input class $class has no inputs" >&2
        return 1
    fi

    for input in "${inputs[@]}"; do
        if [ ! -f "$input" ]; then
            echo "[!] Missing prebuilt input $input" >&2
            return 1
        fi
    done

    prebuilt_sha1sum "${inputs[@]}"
    prebuilt_class_recipe_virtual_entries "$class"
}

prebuilt_class_recipe_key() {
    local manifest

    manifest=$(prebuilt_class_recipe_manifest "$1") || return 1
    printf '%s\n' "$manifest" | prebuilt_sha1sum | awk '{print $1}'
}
```

The package stage first confirms that raw artifacts and recipe inputs exist, then compresses each raw artifact into `.bz2`. When writing `prebuilt.sha1`, the first part contains checksums for each `.bz2` archive, and the second part contains virtual `*.recipe-key` entries generated from class recipe keys. For example, the recipe key for the `image` class is written as `Image.recipe-key`, the `rootfs` class as `rootfs.cpio.recipe-key`, and the `test-tools` class as `test-tools.img.recipe-key`.

```bash
write_archive_checksum_entries_for_class() {
    local class=$1
    local artifact

    while IFS= read -r artifact; do
        prebuilt_sha1sum "${artifact}.bz2"
    done < <(source_artifact_class_outputs "$class")
}

write_recipe_key_entries_for_class() {
    local class=$1
    local recipe_key=$2
    local entry

    while IFS= read -r entry; do
        printf '%s  %s\n' "$recipe_key" "$entry"
    done < <(prebuilt_class_recipe_entries "$class")
}
```

This design separates the question of whether the release matches the current recipe from the question of whether downloaded archive bytes match the manifest. The resolver uses recipe keys to decide release freshness and requires the manifest to contain the corresponding `.bz2` checksum entries.

When the materializer downloads a release artifact, it first verifies the `.bz2` checksum against `prebuilt.sha1` and confirms that the `*.recipe-key` in the manifest still matches the resolver plan. It then decompresses the archive and writes stamps. If the release asset itself is stale or corrupt, or if the manifest and asset are inconsistent, CI fails and leaves the rolling release state issue for the maintainer to inspect.

The CI local cache contract is also the recipe key. After the materializer successfully downloads or builds a class, it calls `prebuilt_stamp_artifacts_for_class` and writes the same class recipe key to `.stamps/prebuilt-local/<artifact>.recipe-key`. After a later workflow restores the raw artifact cache, the resolver uses those stamps to decide whether cached raw artifacts are trustworthy.

```bash
prebuilt_local_stamp_dir=.stamps/prebuilt-local

prebuilt_artifact_recipe_stamp() {
    printf '%s/%s.recipe-key\n' "$prebuilt_local_stamp_dir" "$1"
}

prebuilt_stamp_artifacts_for_class() {
    local class=$1
    local recipe_key=$2
    local artifact

    if [ -z "$recipe_key" ]; then
        echo "[!] Cannot stamp $class with an empty recipe key" >&2
        return 1
    fi

    mkdir -p "$prebuilt_local_stamp_dir" || return 1
    while IFS= read -r artifact; do
        if [ ! -f "$artifact" ]; then
            echo "[!] Cannot stamp missing artifact: $artifact" >&2
            return 1
        fi
        printf '%s\n' "$recipe_key" > "$(prebuilt_artifact_recipe_stamp "$artifact")"
    done < <(source_artifact_class_outputs "$class")
}
```

It is important to distinguish `.stamps/prebuilt-local` from local Make. `.stamps/prebuilt-local` belongs to the CI raw cache and records which recipe key produced a raw artifact restored from cache. Local `make check` uses Make file targets and local filesystem state. Developers explicitly decide to rebuild artifacts through `make build-artifacts`.

## CI Decision Model

CI decision-making starts in the `guest-artifact-build` job in `.github/workflows/main.yml`. After checking out the source tree, this job runs `.ci/prebuilt/resolve-artifacts.sh`, which compares the current checkout, the rolling release manifest, and any local stamps already present in the workspace into a plan.

Cache restore and builds happen after this plan. The plan is a plain `KEY=VALUE` file. Later dependency installation, materialization, and publish-gate steps read it.

The resolver first sources `.ci/prebuilt/artifact-inputs.sh` to load the shared artifact contract, CI recipe-key helpers, and plan-variable naming helpers. It then computes `current_*_recipe_key` for every registered class. This key describes what artifact the current checkout needs.

```bash
while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" current_recipe_key \
        "$(prebuilt_class_recipe_key "$class")"
    resolve_local_class "$class"
done < <(source_artifact_classes)
```

In the same pass, the resolver checks whether the workflow-local raw cache has been restored into the workspace. Here, local means the raw artifact cache inside the GitHub Actions workspace. The resolver requires every artifact in a class to exist, every artifact to have a `.stamps/prebuilt-local/*.recipe-key` stamp, and all stamps in the same class to have the same value. If any artifact or stamp is missing, or stamps in the same class disagree, the cache for that class is marked invalid.

```bash
class_local_recipe_key() {
    local class=$1
    local artifact
    local recipe_stamp
    local recipe_key=
    local next=
    local have_key=false

    while IFS= read -r artifact; do
        recipe_stamp=$(prebuilt_artifact_recipe_stamp "$artifact")
        if [ ! -f "$artifact" ] || [ ! -f "$recipe_stamp" ]; then
            return 1
        fi
        IFS= read -r next < "$recipe_stamp" || return 1
        if [ -z "$next" ]; then
            return 1
        fi
        if [ "$have_key" = false ]; then
            recipe_key=$next
            have_key=true
        elif [ "$recipe_key" != "$next" ]; then
            return 1
        fi
    done < <(source_artifact_class_outputs "$class")

    if [ "$have_key" = false ]; then
        return 1
    fi

    printf '%s\n' "$recipe_key"
}
```

Next, the resolver downloads the rolling release `prebuilt.sha1` into a temporary directory. `manifest_class_recipe_key` first confirms that every artifact has a corresponding `.bz2` checksum entry, then reads virtual entries such as `Image.recipe-key`, `rootfs.cpio.recipe-key`, or `test-tools.img.recipe-key`. If a class is missing an archive checksum entry, is missing a recipe-key entry, or has multiple artifact entries in the same class pointing to different keys, that class release key is treated as empty, and later action selection falls back to another artifact source.

```bash
manifest_entry_value() {
    local entry=$1

    awk -v f="$entry" '$2 == f {print $1; found = 1; exit} END {exit !found}' "$manifest"
}

manifest_class_recipe_key() {
    local class=$1
    local artifact
    local next
    local recipe_key=
    local have_key=false

    while IFS= read -r artifact; do
        if ! manifest_entry_value "${artifact}.bz2" >/dev/null; then
            printf '\n'
            return
        fi
        if ! next=$(manifest_entry_value "${artifact}.recipe-key"); then
            printf '\n'
            return
        fi
        if [ "$have_key" = false ]; then
            recipe_key=$next
            have_key=true
        elif [ "$recipe_key" != "$next" ]; then
            printf '\n'
            return
        fi
    done < <(source_artifact_class_outputs "$class")

    printf '%s\n' "$recipe_key"
}
```

After the resolver knows the current recipe key, the workflow-local raw cache recipe key and status, and the rolling release recipe key, it decides the real action. Release and local keys are internal resolver decision state. The materializer plan outputs each class action and current recipe key.

The priority order is workflow-local raw cache, rolling release, then source build. This order matters because the workflow-local raw cache is a raw artifact bundle from the same workflow or nearby workflows. When a stamp matches the current checkout, CI uses the restored cache. When the release matches, CI downloads release artifacts. When both sources miss, CI builds from source.

```bash
decide_action() {
    local current_recipe_key=$1
    local release_recipe_key=$2
    local local_recipe_key=$3
    local local_status=$4

    if [ "$local_status" = valid ] && [ "$local_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' use-action-cache
        return
    fi

    if [ -n "$release_recipe_key" ] && [ "$release_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' download-release
    else
        printf '%s\n' build
    fi
}
```

The final resolver plan keeps the fields required by later steps: each class action, the current recipe key, `requires_build`, `release_needs_update`, `platform_action_cache_tag`, and `ci_cache_schema_tag`. `platform_action_cache_tag` folds the current recipe keys of the three classes into one cache namespace for GitHub Actions cache naming. Artifact correctness is determined by each class action and recipe key.

```bash
printf 'plan_version=1\n'
prebuilt_plan_print_class_values action
prebuilt_plan_print_class_values current_recipe_key
printf 'platform_action_cache_tag=%s\n' "$platform_action_cache_tag"
printf 'ci_cache_schema_tag=%s\n' "$ci_cache_schema_tag"
printf 'requires_build=%s\n' "$requires_build"
printf 'release_needs_update=%s\n' "$release_needs_update"
```

The first workflow resolve is the publish gate. If `release_needs_update=false`, the rolling release matches the current checkout for all classes, so `guest-artifact-build` skips cache restore, materialization, packaging, and upload.

Later test jobs materialize raw artifacts directly from the release through `.github/actions/provide-guest-artifacts`.

If `release_needs_update=true`, the workflow restores the workflow-local raw cache, then runs the resolver again after cache restore to produce the post-cache plan.

```yaml
...
- name: resolve release update plan
  id: resolve
  shell: bash
  env:
    PREBUILT_BOOTSTRAP_ON_404: ${{ github.repository != 'sysprog21/semu' }}
  run: |
    .ci/prebuilt/resolve-artifacts.sh > "$RUNNER_TEMP/prebuilt-plan-main.env"
    .ci/github/append-github-output.sh "$RUNNER_TEMP/prebuilt-plan-main.env" \
      release_needs_update \
      platform_action_cache_tag \
      ci_cache_schema_tag

...

- name: cache workflow-local raw guest artifacts
  if: steps.resolve.outputs.release_needs_update == 'true'
  uses: actions/cache@v5
  ...

- name: materialization plan after cache restore
  id: post_cache_resolve
  if: steps.resolve.outputs.release_needs_update == 'true'
  shell: bash
  env:
    PREBUILT_BOOTSTRAP_ON_404: ${{ github.repository != 'sysprog21/semu' }}
  run: |
    .ci/prebuilt/resolve-artifacts.sh > "$RUNNER_TEMP/prebuilt-plan-post-cache.env"
    .ci/github/append-github-output.sh "$RUNNER_TEMP/prebuilt-plan-post-cache.env" \
      requires_build
...
```

The materializer executes the plan already decided by the resolver. The caller points it at the plan file with `PREBUILT_PLAN_FILE`. `parse_resolver_output` consumes `plan_version`, `*_action`, and `current_*_recipe_key`, and skips scalar fields used by the workflow, such as `requires_build`, `platform_action_cache_tag`, `ci_cache_schema_tag`, and `release_needs_update`. Any other unknown key fails fast so an old materializer cannot silently misunderstand a new plan schema.

```bash
...
if [ -z "${PREBUILT_PLAN_FILE:-}" ]; then
    echo "[!] PREBUILT_PLAN_FILE is required; create a CI resolver plan before materializing." >&2
    exit 1
fi
if [ ! -f "$PREBUILT_PLAN_FILE" ]; then
    echo "[!] PREBUILT_PLAN_FILE does not exist: $PREBUILT_PLAN_FILE" >&2
    exit 1
fi
resolver_output=$(cat "$PREBUILT_PLAN_FILE")

...

parse_resolver_output <<< "$resolver_output"

if [ "$plan_version" != 1 ]; then
    echo "[!] Unsupported prebuilt plan version: ${plan_version:-missing}" >&2
    exit 1
fi
...
```

Before doing any download or build, the materializer verifies that every class has a valid action. If a test job sets `PREBUILT_FORBID_BUILD=1` while the plan still contains a `build` action, the materializer fails immediately. This guard lets test jobs consume workflow artifacts or the rolling release that have already been selected. Publish-state issues encountered during testing are surfaced directly.

```bash
if [ "$requires_build" = true ] && [ "$forbid_build" = true ]; then
    echo "[!] PREBUILT_FORBID_BUILD forbids source builds, but the plan contains:" >&2
    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            printf '[!]   %s=build\n' "$(prebuilt_plan_class_var_name "$class" action)" >&2
        fi
    done < <(source_artifact_classes)
    exit 1
fi
```

When source builds are actually needed, the materializer calls `.ci/prebuilt/build-plan-artifacts.sh`. This internal builder sources `scripts/prebuilt/artifact-recipes.sh` and runs `build_prebuilt_artifact_classes`. The commit point for stamps remains in the materializer because the materializer knows whether the full plan action completed successfully.

After a successful build, the materializer stamps the corresponding class with the current recipe key.

```bash
if [ "$requires_build" = true ]; then
    build_plan_classes
    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            prebuilt_stamp_artifacts_for_class "$class" \
                "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"
        fi
    done < <(source_artifact_classes)
fi
```

For each `download-release` class, the materializer downloads the corresponding `.bz2` through `scripts/prebuilt/download-release-artifact.sh` and verifies the archive checksum and expected recipe key against `prebuilt.sha1`.

After verification succeeds, the materializer decompresses the archive into the raw artifact and writes `.stamps/prebuilt-local`. For each `use-action-cache` class, cache restore has already placed the raw artifact and stamp in the workspace.

The final result of the CI decision model is that every class leaves a raw artifact in the workspace. If the artifact was managed by the CI materializer, it also leaves a matching recipe-key stamp for a later cache restore to evaluate again.

```bash
while IFS= read -r class; do
    if [ "$(prebuilt_plan_get_class_var "$class" action)" = download-release ]; then
        download_release_class "$class" \
            "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"
    fi
done < <(source_artifact_classes)
```

The responsibilities of each layer are therefore:

- The resolver makes decisions and leaves raw artifacts unchanged.
- The materializer executes the plan.
- The package step compresses materialized raw artifacts and writes the archive checksum and recipe-key manifest.
- Test jobs obtain artifacts and run tests.
- The publish job updates the rolling `prebuilt` release after a master push and successful tests for the same artifact set.

## GitHub Workflow

`.github/workflows/main.yml` runs on `push` and `pull_request`. The main artifact job is `guest-artifact-build`.

`guest-artifact-build` runs in this order:

1. Check out the source tree.
2. Run `.ci/prebuilt/resolve-artifacts.sh` against the rolling release manifest.
3. Publish `release_needs_update`, `platform_action_cache_tag`, and `ci_cache_schema_tag` as step outputs.
4. If the rolling release already matches the current recipe keys, skip materialization, packaging, and upload in this job.
5. If the release is stale, restore the workflow-local raw artifact cache keyed by `platform_action_cache_tag` and `ci_cache_schema_tag`.
6. Run the resolver again after cache restore.
7. If the post-cache plan still contains a `build` action, install Buildroot/Linux build dependencies.
8. Run `.ci/prebuilt/materialize-artifacts.sh` with the post-cache plan.
9. Run `.ci/prebuilt/package.sh`.
10. Upload the raw artifacts for test jobs and the packaged artifacts for publish.

The workflow-local raw artifact cache contains:

- `Image`
- `rootfs.cpio`
- `test-tools.img`
- `.stamps/prebuilt-local/`

Master pushes also restore this cache. The publish decision is separate: master can publish after all required tests pass on the same materialized artifacts.

Here, `release_needs_update` represents artifact recipe-key drift and release-contract drift explicitly checked by the resolver, such as the requirement that `prebuilt.sha1` contain archive checksum entries. Changes to `.ci/prebuilt/package.sh`, `.ci/prebuilt/verify-package.sh`, release transport, or release layout need to make the resolver observe stale state from the current release manifest, or explicitly add schema to the gate, before they can automatically trigger a republish.

Forks and mirrors may not have their own rolling `prebuilt` release yet. The GitHub workflow enables `PREBUILT_BOOTSTRAP_ON_404` for non-canonical repositories, so an HTTP 404 for `prebuilt.sha1` becomes a first-run source build. In the canonical `sysprog21/semu` repository, HTTP 404 is a failure and asks the maintainer to inspect external release state.

This bootstrap exception applies to HTTP 404. DNS failures, connection timeouts, HTTP 5xx, and other non-404 errors are external release-state or network-state problems. The resolver fails directly.
