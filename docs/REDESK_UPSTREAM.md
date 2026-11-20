# ReDesk upstream integration

ReDesk is a downstream fork of [CrossDesk](https://github.com/kunkundi/crossdesk).

## Baseline

- Initial tested source: CrossDesk `v1.7.6-20261112` at `5f526cd70e48818ee98d153c54c08d4ce95f463b`.
- ReDesk `main` starts at that source commit. Do not force-reset it to CrossDesk's older `main` branch.
- CrossDesk's active default upstream branch on 2026-11-20 is `mcp`. The similarly named upstream `main` was substantially older when this fork was created.
- MiniRTC is a Git submodule; always update it to the exact commit recorded in each imported CrossDesk revision. Use `git submodule update --init --recursive` after checkout.

## Upstream integration policy

1. Fetch upstream tags and the active upstream branch.
2. Select a reviewed release tag or a narrowly scoped bug-fix commit. Do **not** automatically merge all upstream changes.
3. Create a temporary `sync/crossdesk-<version>` branch from ReDesk `main`.
4. Merge the chosen upstream revision, preserving history, or cherry-pick a specific fix. Never force-push `main`.
5. Resolve conflicts while preserving ReDesk's branding, English new-install default, host/viewer behavior, settings isolation, and independent signaling configuration.
6. Verify submodule commit pointers and license notices; inspect changes to signaling, identity/authentication, transport encryption, updater, installer, and service behavior.
7. Run build and regression tests before proposing a pull request to ReDesk `main`.
8. Record the source CrossDesk revision and MiniRTC revision in the ReDesk release notes.

The upstream CrossDesk updater and its signing key must **not** automatically install upstream CrossDesk binaries on top of a ReDesk build. Disable it until ReDesk-controlled signed releases are implemented.

## Windows build compatibility

On Windows, Visual Studio 2026 / MSVC 19.52 exposed a preprocessor incompatibility in dav1d 1.4.3. Upstream dav1d has an official [fix](https://github.com/videolan/dav1d/commit/a34e0685b9111b9eb1d7835ea7bea5b425dced73) for `__has_include_next`.

An Xmake usrsctp dependency recipe also encountered an upstream patch checksum mismatch. CrossDesk upstream has since advanced its MiniRTC submodule to incorporate bundled usrsctp patches. Those changes still need an isolated verification build before being imported into ReDesk. Do not disable checksum validation globally.

Temporary changes to locally installed Xmake package repositories or dependency caches are **not** yet part of this repository. Make any required build corrections reproducible in version-controlled source before release.

## Localization

For a fresh ReDesk desktop installation, English is the default regardless of Windows UI language. Existing saved language choices continue to take precedence. Keep the upstream localization catalog and its generator intact.

## Licensing

CrossDesk is GPL-3.0-only and MiniRTC has separate LGPL licensing. Preserve attribution, applicable source-distribution obligations, and third-party notices in ReDesk distributions.
