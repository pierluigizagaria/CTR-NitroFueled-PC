# P2P channel A

`p2p/channel-a` is a downstream integration branch based on the SacredFire
`v0.4` release (`0cf8def7f676192de4f37a26e08a92ea0cd4674a`). It includes the
optional PSN backend interface proposed in upstream pull request
[GetLostIn/CTR-NitroFueled-PC#79](https://github.com/GetLostIn/CTR-NitroFueled-PC/pull/79).
The upstream project, contributors and GPL-2.0-or-later license remain identified
in the source and distribution. Channel names identify downstream builds; they
do not imply upstream support or endorsement.

## Runtime contract

The independent `ctrnf-p2p` supervisor starts the native launcher with the PSN
bridge backend selected. It supplies these process environment values:

- `BB_PSN_BACKEND`: the absolute path of `psn-bridge-backend.dll`.
- `BB_PSN_BACKEND_CONFIG`: the loopback relay URL and a fresh session capability,
  using the JSON contract in `platform/psn/README.md`. Keep the capability in the
  process environment; do not persist it in configuration or release metadata.
- `BB_ONLINE_SERVER` and `BB_ONLINE_HTTP_PORT`: the private publisher address and
  HTTP port. Selecting a PSN backend does not enable private-server networking.
- `BB_ONLINE_ID`: the supervisor's session identity, also provided by the selected
  backend through the PSN API.

The launcher and `run.py` preserve these inherited values. The selected backend
owns PSN identity, authentication, friends, invitations and WebAPI forwarding;
the native port keeps its local-user ABI, trophies, game preparation, settings,
saves, socket implementation and HTTP routing. The backend fails startup if its
relay handshake fails. There is no fallback from a selected backend to the
legacy fabricated identity.

The native socket policy already permits the publisher and all IPv4 loopback
addresses (`127.0.0.0/8`), including the provider's `loopback-v1` endpoints. No
additional peer address allowlist or host transport implementation is needed for
that endpoint scheme. The supervisor must enable and validate the independent
server-signature patch before launch when publisher replies are unsigned.

The host remains usable without the optional backend. Its existing offline and
private-server defaults are unchanged. The PSN module contains no Steam provider
or publisher implementation; those components are built and licensed separately.

## Windows build

Use the MSYS2 CLANG64 dependencies documented in `packaging/windows/README.md`.
Build the complete host and its native tests:

```sh
bash build.sh --test
python3 -B -m unittest discover -s tests
```

Build and test the optional bridge backend with libcurl 7.85 or later and
nlohmann-json 3.11 or later available to CMake:

```sh
cmake -S platform/psn -B out/psn-bridge -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPSN_BRIDGE_BACKEND=ON
cmake --build out/psn-bridge
ctest --test-dir out/psn-bridge --output-on-failure
```

If nlohmann-json has no CMake package, pass
`-DPSN_JSON_INCLUDE_DIR=/path/to/include`; this is an ordinary external build
dependency, not a dependency on another project's source tree. The backend's
build copies its required non-system DLLs beside it on Windows. Include that
complete dependency set and its license notices in the combined package.

The native packager produces `CTR-NitroFueled-PC-portable-windows.zip` with
`CTR-NitroFueled-PC.exe` at its root and `bin/sacredfire.exe` as the runtime. The
combined release adds the supervisor, provider, publisher and backend to this
complete native distribution. Pin every component revision, include matching
source and notices, and retain the native package's portable Python and runtime
dependencies. Do not include games, firmware, generated game images, user
settings, saves, databases, credentials or runtime session capabilities.

This branch also omits the upstream branded logo/icon and the original cached
game shaders formerly bundled for ultrawide support. Ultrawide reads and checks
the player's own warm shader cache, saving verified originals under
`user/ultrawide-shader-originals/<title>/` before applying corrections. Both
aspect modes share these private backups, and returning to 16:9 restores them.
An older corrected cache without a verified original must be restored locally
before enabling ultrawide; unknown shader modifications are preserved. A normal
16:9 launch needs no bundled shader cache.

Native and backend fixture tests validate the host/API contract. They do not
establish successful game login, reciprocal invitations or a playable P2P race;
those require a separate game session with the exact published package.
