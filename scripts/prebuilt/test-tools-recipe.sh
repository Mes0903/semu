#!/usr/bin/env bash

# Build-layer helper for selecting optional test-tools.img recipe entries. CI
# recipe keys source this same helper so recipe selection affects both local
# builds and CI cache/release decisions without making the build recipe depend
# on CI state.
prebuilt_test_tools_recipe_entries() {
    local raw=${PREBUILT_TEST_TOOLS_RECIPE+x}
    local recipe=${PREBUILT_TEST_TOOLS_RECIPE-}
    local item
    local want_x11=false
    local want_directfb2=false

    if [ -z "$raw" ]; then
        printf "%s\n" x11 directfb2
        return 0
    fi

    recipe=${recipe//,/ }
    if [ -z "${recipe// /}" ]; then
        echo "[!] Empty test-tools recipe" >&2
        echo "[!] Expected recipe entries: x11, directfb2" >&2
        return 1
    fi

    for item in $recipe; do
        case "$item" in
            x11)
                want_x11=true
                ;;
            directfb2)
                want_directfb2=true
                ;;
            *)
                echo "[!] Unknown test-tools recipe entry: $item" >&2
                echo "[!] Expected recipe entries: x11, directfb2" >&2
                return 1
                ;;
        esac
    done

    if [ "$want_x11" = true ]; then
        printf "%s\n" x11
    fi
    if [ "$want_directfb2" = true ]; then
        printf "%s\n" directfb2
    fi
}

prebuilt_test_tools_recipe_includes() {
    local want=$1
    local entry
    local recipe_entries

    recipe_entries=$(prebuilt_test_tools_recipe_entries) || return 1
    while IFS= read -r entry; do
        if [ "$entry" = "$want" ]; then
            return 0
        fi
    done <<< "$recipe_entries"

    return 1
}

prebuilt_test_tools_recipe_key() {
    local entry
    local recipe_entries
    local result=

    recipe_entries=$(prebuilt_test_tools_recipe_entries) || return 1
    while IFS= read -r entry; do
        if [ -z "$entry" ]; then
            continue
        fi
        if [ -n "$result" ]; then
            result=$result,$entry
        else
            result=$entry
        fi
    done <<< "$recipe_entries"

    if [ -z "$result" ]; then
        echo "[!] Empty test-tools recipe" >&2
        return 1
    fi

    printf "%s\n" "$result"
}
