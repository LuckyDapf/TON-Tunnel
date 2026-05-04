# Contributing

1. Fork the repository and create a branch from `main`.
2. Keep commits focused — one logical change per pull request when possible.
3. Match existing code style (C++20 for native code; Kotlin idioms for Android).
4. For native changes touching `third_party/ton`, prefer upstream-style patches so merging vendor updates stays feasible.
5. **Do not** commit local `build/`, `.gradle/`, or generated artifacts (see `.gitignore`).
6. Update `BUILD.md` / `PROTOCOL.md` if you change observable behavior or APIs.

Legal: by submitting a pull request you confirm your contribution can be licensed under **AGPL-3.0** (same terms as `LICENSE`). If you introduce third-party snippets, cite the upstream license.

Russian version: [`CONTRIBUTING.ru.md`](CONTRIBUTING.ru.md). Code of conduct: [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) · [`CODE_OF_CONDUCT.ru.md`](CODE_OF_CONDUCT.ru.md). Documentation map: [`.github/DOCUMENTATION.md`](.github/DOCUMENTATION.md).
