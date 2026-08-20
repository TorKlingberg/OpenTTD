# Implementation Brief: Community Builds and Browser Demo

You are working in my fork of OpenTTD:

`TorKlingberg/OpenTTD`

The fork contains a large experimental feature implementing modular airports.

This project is not intended to be submitted upstream to OpenTTD. I want to make the fork easy for community members to try.

Implement two fork-specific GitHub Actions workflows:

1. A manually triggered Emscripten build deployed to GitHub Pages.
2. A manually triggered native build that publishes downloadable binaries as a GitHub pre-release.

Please inspect the current repository and existing OpenTTD workflows before making changes. Reuse their tested build logic where practical, but do not inherit OpenTTD-specific publishing infrastructure.

## General constraints

Do not use or require any OpenTTD-owned secrets or services.

Specifically, the new workflows must not depend on:

- OpenTTD Cloudflare credentials
- OpenTTD CDN credentials
- Steam credentials
- GOG credentials
- Azure/Windows code-signing credentials
- Apple Developer signing or notarization credentials
- survey signing credentials

Do not modify gameplay code as part of this task.

Do not replace or substantially rewrite OpenTTD's existing general CI/release infrastructure unless a very small compatibility change is genuinely necessary.

Prefer adding clearly named fork-specific workflows.

Do not trigger `.github/workflows/release.yml` as part of the publishing process. That workflow contains OpenTTD's official CDN/Steam/GOG publishing path and is inappropriate for this fork.

Keep security permissions minimal. Give write permissions only to the jobs that actually publish something.

The workflows must be usable entirely from the GitHub web UI after they have been committed to the default branch.

---

# Part 1 — Browser demo on GitHub Pages

Create a workflow such as:

`.github/workflows/community-web.yml`

Display name:

`Publish web demo`

## Trigger

Use `workflow_dispatch`.

Add a string input:

`ref`

Default:

`master`

The workflow must build the requested ref, not simply assume that the workflow's own checked-out revision is the game revision to build.

Do not deploy automatically on every push for now. I want publishing to be deliberate.

## Build implementation

Inspect:

- `.github/workflows/preview-build.yml`
- `.github/workflows/ci-emscripten.yml`
- `os/emscripten/README.md`
- `os/emscripten/Dockerfile`

Use the existing OpenTTD Emscripten build recipe rather than inventing a new one.

At the time this task was written, `preview-build.yml` used Emscripten SDK 3.1.57 and:

1. checked out the source;
2. installed the expected GCC version;
3. cached the Emscripten cache;
4. installed the repository's LibLZMA Emscripten port;
5. built host tools with `OPTION_TOOLS_ONLY=ON`;
6. configured the target with `emcmake`;
7. built the `openttd` target;
8. collected:
   - `openttd.html`
   - `openttd.js`
   - `openttd.wasm`
   - `openttd.data`.

Verify the repository's current versions and commands instead of assuming these have not changed.

Do not use the Cloudflare publishing step from `preview-build.yml`.

## Pages artifact

Create a `public` directory containing the complete browser build.

It should contain at least:

- `openttd.html`
- `openttd.js`
- `openttd.wasm`
- `openttd.data`

Also make the root project URL directly playable.

The simplest approach is to copy:

`openttd.html`

to:

`index.html`

Retain `openttd.html` as well unless there is a reason not to.

Add `.nojekyll` if appropriate.

Do not rewrite generated asset URLs unnecessarily. Verify that the generated page works when served from a GitHub Pages project path such as `/OpenTTD/`, rather than assuming the application is hosted at the domain root.

## Pages deployment

Use GitHub's standard Pages Actions.

Use current compatible versions of:

- `actions/configure-pages`
- `actions/upload-pages-artifact`
- `actions/deploy-pages`

Use a separate deployment job if that makes the containerized Emscripten build more robust.

The deployment job must use the `github-pages` environment and expose the Pages URL as the deployment environment URL.

Use minimal permissions. The deployment path will require at least:

```yaml
permissions:
  contents: read
  pages: write
  id-token: write
```

Use appropriate concurrency so that two simultaneous Pages deployments cannot corrupt or race each other. A newer deployment may cancel an older in-progress Pages deployment.

The workflow must require no repository secrets.

## Browser-workflow acceptance criteria

The workflow can be launched from:

**Actions → Publish web demo → Run workflow**

with `ref=master`.

A successful run must deploy a playable page through GitHub Pages.

The site's root URL must load the game without requiring `/openttd.html` to be manually appended.

The browser developer console/network panel should not show missing `.js`, `.wasm`, or `.data` files caused by the repository being served below `/OpenTTD/`.

---

# Part 2 — Native downloadable community builds

Create a second workflow such as:

`.github/workflows/community-release.yml`

Display name:

`Publish community build`

## Trigger and inputs

Use `workflow_dispatch`.

Provide these inputs:

### `ref`

Required string.

Default:

`master`

This is the source revision to build.

### `tag`

Required string.

Example:

`modular-airports-test-1`

The workflow will create this tag/release only after the required builds have succeeded.

Validate that the tag is non-empty and does not already name an incompatible existing release.

### `title`

Optional string.

Example:

`Modular Airports test build 1`

If it is empty, derive a sensible release title from `tag`.

## Native build strategy

First inspect these existing workflows:

- `.github/workflows/release-source.yml`
- `.github/workflows/release-linux.yml`
- `.github/workflows/release-windows.yml`
- `.github/workflows/release-macos.yml`
- `.github/workflows/release.yml`

The existing platform workflows already contain OpenTTD's dependency setup, build configuration and CPack packaging logic. Reuse that work where it is clean and safe.

However, the community workflow must not progress into:

- `upload-cdn.yml`
- `upload-steam.yml`
- `upload-gog.yml`
- Windows Store publishing
- any other official OpenTTD distribution channel.

Do not simply invoke the existing top-level `release.yml`.

## Source preparation

If practical, reuse `release-source.yml` so the native platform workflows receive their expected `internal-source` artifact.

Be aware that `release-source.yml` currently obtains a manually selected ref from the `workflow_dispatch` event context.

Make sure that behavior still works when it is invoked by the new wrapper workflow.

If reusing it creates awkward coupling, implement a small fork-specific source-preparation job instead. Prefer correctness and clarity over clever reuse.

## Windows

Windows x64 is the required Windows target.

A portable ZIP is preferred for community testing because it does not pretend to be a signed official installer.

The current OpenTTD Windows workflow performs signing only under certain release/tag conditions and expects Azure signing secrets for that path.

Do not attempt to sign the community binaries.

If reusing `release-windows.yml`, make sure the signing step cannot run.

If the reusable workflow necessarily builds x86 and ARM64 alongside x64, that is acceptable initially, although only the useful packages need to be attached to the release.

Prefer publishing the portable x64 ZIP over an unsigned installer executable if both are generated.

Do not silently label an unsigned package as signed.

## Linux

Build OpenTTD's generic Linux package using the existing generic Linux release recipe where practical.

Publish the useful end-user package produced by CPack.

Do not publish symbol/debug artifacts as ordinary user downloads.

## macOS

Attempt to produce the existing universal macOS package.

It must not require Apple signing/notarization credentials.

The inherited workflow already contains some fork-aware behavior around missing signing/notarization credentials, but verify that the actual unsigned CPack/package path works from beginning to end.

If the existing reusable macOS workflow fails solely because it assumes a signing identity during package creation, make the smallest reasonable change necessary to support an unsigned community-build mode while preserving its existing default behavior.

For example, an optional reusable-workflow input such as an unsigned/community-build flag is preferable to deleting upstream signing support.

If reliable unsigned macOS packaging cannot be achieved without a disproportionately large change, document that clearly and leave macOS out of the first community-release workflow rather than introducing a fragile hack.

Windows x64 and Linux are more important than macOS for the first iteration.

## Release assembly

Do not publish a release until the required build jobs have succeeded.

Download the useful build artifacts into a staging directory.

Exclude:

- Breakpad symbols
- PDB/debug-symbol collections
- intermediate source artifacts
- duplicate metadata files
- installers or packages that are not meant for ordinary testers

Include, where available:

- Windows x64 portable ZIP
- Linux generic package
- macOS universal ZIP
- source ZIP

Create:

`SHA256SUMS`

covering all binary/source assets actually attached to the release.

## Creating the release

Use the repository's built-in `GITHUB_TOKEN`; do not require a PAT.

The release-creation/upload job should have:

```yaml
permissions:
  contents: write
```

Use the GitHub CLI (`gh`) or another well-supported mechanism to create the release and upload its assets.

Create the tag/release against the exact requested `ref`.

Mark every release created by this workflow as a **pre-release**.

Release creation should happen after successful compilation so that a failed build does not normally leave behind an apparently usable release.

If release creation partially succeeds and uploading an asset fails, make the failure visible rather than silently reporting success.

GitHub suppresses ordinary follow-on workflow events produced using the repository `GITHUB_TOKEN`. Use that normal behavior; do not introduce a PAT merely to trigger other release workflows.

In particular, we do **not** want creation of this community release to start OpenTTD's inherited `release.yml`.

## Release notes

Use a short generated release body along these lines:

```markdown
## Modular Airports experimental build

This is an unofficial experimental OpenTTD fork for testing modular airports.

This build is intended for community testing and is not an official OpenTTD release.

Source ref: `<ref>`
Source commit: `<full SHA>`

The Windows and macOS packages, if present, are not official OpenTTD signed/notarized releases.

Please report modular-airport feedback through the project/community discussion linked from the repository.
```

Do not claim that binaries are signed unless they actually are.

Do not describe the project as an official OpenTTD release.

## Release-workflow acceptance criteria

From the GitHub web interface I must be able to open:

**Actions → Publish community build → Run workflow**

and supply:

```text
ref: master
tag: modular-airports-test-1
title: Modular Airports test build 1
```

After successful required builds, the workflow must:

1. create a Git tag at the selected source revision;
2. create a GitHub pre-release;
3. attach the expected end-user packages;
4. attach `SHA256SUMS`;
5. avoid invoking OpenTTD's CDN, Steam, GOG, Windows Store or other official publishing machinery;
6. require no custom repository secrets.

The release page should be enough for a non-developer to identify and download the Windows x64 build without opening the Actions run.

---

# Part 3 — Documentation

Add a small fork-specific documentation file, for example:

`docs/community-builds.md`

Keep it concise.

Document:

## Browser publishing

```text
GitHub → Actions → Publish web demo → Run workflow
ref = master
```

and note that GitHub Pages must have:

```text
Settings → Pages → Source → GitHub Actions
```

## Binary publishing

```text
GitHub → Actions → Publish community build → Run workflow
ref = master
tag = modular-airports-test-N
title = Modular Airports test build N
```

Explain that releases are deliberately marked as prereleases and that Windows/macOS packages may be unsigned.

Do not add instructions involving OpenTTD's private release infrastructure.

---

# Part 4 — Validation before finishing

Before considering the change complete:

1. Validate the YAML syntax of every modified workflow.
2. Review every `${{ ... }}` expression for correct GitHub Actions context and quoting.
3. Verify that manual workflows are defined on the default branch.
4. Verify that the Pages workflow uses only GitHub-provided Pages infrastructure and no Cloudflare secrets.
5. Verify that the release workflow cannot call the existing CDN/Steam/GOG publishing jobs.
6. Verify that native packaging does not accidentally enter Windows or Apple signing paths.
7. Verify artifact names/patterns against what the current OpenTTD platform workflows actually produce.
8. Make sure release asset selection excludes symbols and intermediate artifacts.
9. Make sure the release job has `contents: write` but build jobs do not receive unnecessary write permissions.
10. Make sure the Pages deployment has `pages: write` and `id-token: write`.
11. Summarize every file created or modified.
12. Explicitly call out anything that cannot be tested locally and requires the first real GitHub Actions run.

Do not create or publish a real GitHub release as part of implementing the code unless I explicitly ask you to do so.

Do not deploy a real Pages site as part of implementation unless I explicitly ask you to do so.

The desired result is code committed or ready to commit; I will perform the first actual publishing runs through the GitHub web UI.