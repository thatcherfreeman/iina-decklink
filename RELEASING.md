# Releasing

The helper is signed with a Developer ID, notarized by Apple, and published
**locally** — this runs entirely on your machine using your local keychain;
no certificate or password is ever stored in the repo or in GitHub Actions.
CI (`.github/workflows/ci.yml`) still builds the helper on every push as a
compile check and runs the native/plugin smoke tests, but it never signs,
notarizes, or publishes — see the comment at the top of that file.

Only arm64 (Apple Silicon) is built and published; the scripts assume
`/opt/homebrew` and the arm64 tarball name. An Intel Mac would need its own
build (`scripts/build_native.sh` itself has no arm64-only restriction — only
`release_macos.sh`/`gh_release_macos.sh`'s conventions assume it).

## One-time setup

1. Have a **Developer ID Application** certificate in your login keychain.
   List your identities:

   ```sh
   security find-identity -v -p codesigning
   ```

2. Store notarization credentials in your keychain as a named profile. The
   password is an **app-specific password** generated at appleid.apple.com —
   not your Apple ID password.

   ```sh
   xcrun notarytool store-credentials "iina-decklink-notary" \
       --apple-id you@example.com --team-id TEAMID --password abcd-efgh-ijkl-mnop
   ```

3. Build the decode-only LGPL FFmpeg once (see README's "FFmpeg licensing" —
   this is also what avoids a Homebrew ffmpeg's GPLv3 obligation landing on
   the release tarball):

   ```sh
   scripts/build_ffmpeg_lgpl.sh
   ```

4. Copy the env template and fill in your identity / profile name:

   ```sh
   cp release.env.example release.env
   # edit release.env
   ```

   `release.env` is gitignored, so your signing identity never gets committed
   or sent to GitHub.

## Cutting a release

1. Bump the version yourself — edit `"version"` in `plugin/Info.json` — and
   commit it:

   ```sh
   git commit -am "Bump version to 0.2.0"
   ```

   This is the one thing not automated: the tag `make release` creates has to
   point at a commit that actually contains the version it's tagging, so
   nothing here stamps the file for you or commits on your behalf.

2. Run:

   ```sh
   make release
   ```

   This builds the helper, bundles the LGPL FFmpeg libraries, signs
   everything with your Developer ID (including the library-validation
   entitlement `DeckLinkAPI.framework` needs — see
   `scripts/release.entitlements`), submits it for notarization and waits for
   Apple, then tags `v0.2.0`, pushes it (which triggers CI's compile check),
   **waits for that CI run to finish** (streaming its progress), and
   creates/updates the GitHub release with the signed tarball and the plugin
   package — printing the final list of attached assets. If CI fails, it
   still publishes the locally-built artifacts (since they already built and
   ran fine here) but exits non-zero with a warning — check what CI caught.

   `scripts/package.sh` prints a `spctl` check partway through; a clean
   release should say `accepted` / `source=Notarized Developer ID`. Safe to
   re-run start to finish — re-uploads overwrite via `--clobber`, and a clean
   working tree with nothing new to commit is exactly the state step 1 above
   already leaves you in.

   `make release` is `scripts/release_macos.sh` (build/sign/notarize) then
   `scripts/gh_release_macos.sh` (tag/push/publish) — run those directly if
   you want to redo just one half, e.g. re-publish after step 2 fails partway
   without rebuilding.

## Notes

- Local unsigned/ad-hoc builds (`scripts/build_native.sh`, or
  `scripts/package.sh` with no `SIGN_IDENTITY`) need no certificate at all —
  only `release_macos.sh` does.
- The `com.apple.security.cs.disable-library-validation` entitlement
  (`scripts/release.entitlements`) is required so the notarized, hardened-
  runtime helper can still `dlopen` Blackmagic's `DeckLinkAPI.framework`,
  which is signed by a different Team ID. Without it, notarization succeeds
  and the helper then finds zero devices on every machine it runs on — ported
  from youtube-decklink, which hit the same issue for the same reason.
- The release artifact is a bare binary + `.dylib`s in a `.tar.gz`, not an
  `.app`/`.pkg`/`.dmg` — `stapler staple` only attaches to those container
  formats, so this isn't stapled. Gatekeeper instead confirms the
  notarization ticket online on first launch, the normal path for a
  notarized command-line tool. That first launch already needs network (the
  file was just downloaded from GitHub), so this costs nothing in practice.
- Users still need Blackmagic **Desktop Video** installed at runtime; the
  helper `dlopen`s the framework from `/Library/Frameworks` and does not
  bundle it.
