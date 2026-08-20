# Community builds (fork-specific)

This fork adds two manually-triggered GitHub Actions workflows for sharing
the modular airports feature with testers. Both are fork-specific: they do
not touch upstream OpenTTD's CDN, Steam, GOG, Windows Store, or any signing
infrastructure, and neither requires any repository secrets.

## Browser demo (GitHub Pages)

```
GitHub -> Actions -> Publish web demo -> Run workflow
ref = master
```

This builds the Emscripten/WebAssembly version of the game and deploys it to
GitHub Pages. The run's summary page and the "github-pages" deployment
environment both link to the published URL.

One-time setup, before the first run:

```
Settings -> Pages -> Source -> GitHub Actions
```

Without this, the deploy job fails with a clear error instead of silently
doing nothing.

Each run cancels any Pages deployment still in progress from a previous run,
so it's safe to re-trigger without waiting.

## Native binaries (GitHub pre-release)

```
GitHub -> Actions -> Publish community build -> Run workflow
ref = master
tag = modular-airports-test-N
title = Modular Airports test build N
```

`tag` must not already belong to an existing release; pick a new value each
time (e.g. increment `N`). `title` is optional — if left blank, a title is
derived from the tag.

This builds Windows (x64), Linux (generic), and macOS, then attaches:

- Windows x64 portable ZIP
- Linux generic package
- macOS universal ZIP (if the build succeeds)
- Source ZIP
- `SHA256SUMS`

to a new GitHub **pre-release** at the exact commit `ref` resolved to. The
release is only created after all builds succeed.

Important notes for testers:

- **Every release from this workflow is marked as a pre-release.** These are
  unofficial, unsigned experimental builds — not official OpenTTD releases.
- **The Windows build is never signed** (no Azure code-signing credentials
  are used), and it ships as a portable ZIP rather than an installer.
- **The macOS build is not notarized** (no Apple Developer credentials are
  used). macOS may warn about an unidentified developer on first launch.
- Report modular-airport feedback through the project/community discussion
  linked from the repository, not through upstream OpenTTD's channels.
