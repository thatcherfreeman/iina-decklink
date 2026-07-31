#!/usr/bin/env bash
# Publish a release: tags vVERSION, pushes it, waits for CI's compile check
# to confirm the tagged commit actually builds, then creates/updates the
# GitHub release with the locally signed & notarized artifacts.
#
# Run scripts/release_macos.sh first to produce them.
set -euo pipefail
cd "$(dirname "$0")/.."

WORKFLOW="ci.yml"

# A dirty working tree means the version this reads from plugin/Info.json
# might not be what's actually at HEAD — the tag would then point at a commit
# that doesn't agree with its own release notes/filenames. Catch that here
# rather than let it surface later as "the tag says 0.2.0 but Info.json in
# that commit still says 0.1.0."
if [[ -n "$(git status --porcelain)" ]]; then
    echo "error: working tree has uncommitted changes." >&2
    echo "  If you just bumped the version in plugin/Info.json, commit it first," >&2
    echo "  e.g.:  git commit -am 'Bump version to X.Y.Z'" >&2
    exit 1
fi

VERSION="$(python3 -c 'import json; print(json.load(open("plugin/Info.json"))["version"])')"
TAG="v${VERSION}"
ARCH="$(uname -m)"
TARBALL="dist/iina-decklink-helper-${ARCH}.tar.gz"
PLGZ="dist/iina-decklink.iinaplgz"

if [[ ! -f "$TARBALL" || ! -f "$PLGZ" ]]; then
    echo "error: $TARBALL and/or $PLGZ not found — run ./scripts/release_macos.sh first." >&2
    exit 1
fi

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
HEAD_SHA="$(git rev-parse HEAD)"

# The release is only meaningful if what's on GitHub actually matches what
# you're about to tag. Refuse to guess — push the branch yourself first.
# --verify -q avoids a git rev-parse gotcha: without --verify, an unresolved
# ref still exits non-zero but ALSO echoes the literal argument to stdout,
# which "|| true" would otherwise capture as if it were a real SHA.
UPSTREAM_SHA="$(git rev-parse --verify -q "refs/remotes/origin/$BRANCH" 2>/dev/null || true)"
if [[ -z "$UPSTREAM_SHA" ]]; then
    echo "error: origin/$BRANCH doesn't exist — push $BRANCH first:" >&2
    echo "  git push -u origin $BRANCH" >&2
    exit 1
fi
if [[ "$UPSTREAM_SHA" != "$HEAD_SHA" ]]; then
    echo "error: local $BRANCH ($HEAD_SHA) is not what's on origin/$BRANCH ($UPSTREAM_SHA)." >&2
    echo "  This release script tags and publishes the current commit, so GitHub" >&2
    echo "  needs to already have it (that's also what CI will check). Push first:" >&2
    echo "  git push origin $BRANCH" >&2
    exit 1
fi

just_pushed=false
existing_tag_sha="$(git rev-parse --verify -q "refs/tags/$TAG" 2>/dev/null || true)"
if [[ -n "$existing_tag_sha" && "$existing_tag_sha" != "$HEAD_SHA" ]]; then
    echo "error: tag $TAG already exists but points at $existing_tag_sha," >&2
    echo "  not the current commit ($HEAD_SHA). Probably left over from an" >&2
    echo "  earlier attempt. To move it (only safe if no one else is relying" >&2
    echo "  on the existing tag/release):" >&2
    echo "    git tag -d $TAG && git push origin :refs/tags/$TAG" >&2
    echo "    ./scripts/gh_release_macos.sh" >&2
    exit 1
elif [[ -n "$existing_tag_sha" ]]; then
    echo "Tag $TAG already exists and points at the current commit."
    if [[ -z "$(git ls-remote --tags origin "refs/tags/$TAG")" ]]; then
        echo "  ...but isn't on origin yet — pushing it now."
        git push origin "$TAG"
        just_pushed=true
    fi
else
    echo "Tagging $TAG and pushing (triggers CI's compile check)..."
    git tag "$TAG"
    git push origin "$TAG"
    just_pushed=true
fi

# Find the CI run for THIS commit specifically (matched by headSha, not just
# branch/tag name) — a same-named tag from an earlier attempt can have its own
# old run still in the list, which "most recent for this branch" alone can't
# reliably distinguish from a genuinely fresh one, especially right after a
# push before the new run has been indexed yet. If we just pushed, a fresh run
# should appear within a few seconds; poll briefly for it. If the tag already
# existed and pointed here, there may be no run to wait on (e.g. re-publishing
# after an upload failure) — skip straight to uploading in that case.
run_id=""
attempts=0
max_attempts=$([[ "$just_pushed" == true ]] && echo 20 || echo 1)
while [[ -z "$run_id" && $attempts -lt $max_attempts ]]; do
    run_id="$(gh run list --workflow "$WORKFLOW" --commit "$HEAD_SHA" --limit 1 \
        --json databaseId --jq '.[0].databaseId // empty' 2>/dev/null || true)"
    if [[ -z "$run_id" ]]; then
        attempts=$((attempts + 1))
        sleep 3
    fi
done

ci_failed=false
if [[ -n "$run_id" ]]; then
    echo "Watching CI run $run_id for $TAG (compile check + native/plugin tests)..."
    if ! gh run watch "$run_id" --exit-status --compact; then
        ci_failed=true
        echo "warning: CI run $run_id did not succeed. Publishing the locally" >&2
        echo "  signed build anyway, since it built and ran fine here, but" >&2
        echo "  check what CI caught: gh run view $run_id" >&2
    fi
else
    echo "No CI run found for $TAG (nothing to wait for)."
fi

if gh release view "$TAG" >/dev/null 2>&1; then
    echo "Uploading signed macOS build to release $TAG..."
    gh release upload "$TAG" "$TARBALL" "$PLGZ" --clobber
else
    echo "Creating release $TAG..."
    gh release create "$TAG" "$TARBALL" "$PLGZ" \
        --title "$TAG" \
        --notes "Signed and notarized. Requires Blackmagic Desktop Video."
fi

echo
echo "Release assets for $TAG:"
gh release view "$TAG" --json assets --jq '.assets[].name' | sed 's/^/  /'
echo
echo "https://github.com/$(gh repo view --json nameWithOwner -q .nameWithOwner)/releases/tag/$TAG"

if [[ "$ci_failed" == true ]]; then
    exit 1
fi
