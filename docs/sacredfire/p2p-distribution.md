# P2P distribution integration

`p2p/channel-b` tracks upstream releases and keeps the optional PSN adapter as a
small local change. This branch is based on upstream `v0.6`
(`26b35630f62c6ea233acb87aae3fe1f70b89034c`). The original contributor identities,
GPL-2.0-or-later license, and third-party notices remain applicable.

Build the Windows runtime and launcher with the existing MSYS2 CLANG64 toolchain:

```sh
BB_P2P_DISTRIBUTION=ON bash build.sh --test
cmake -S platform/psn -B out/psn-bridge-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DPSN_BRIDGE_BACKEND=ON -DPSN_HOST_TESTS=ON
cmake --build out/psn-bridge-tests
ctest --test-dir out/psn-bridge-tests --output-on-failure
BB_P2P_DISTRIBUTION=ON python packaging/windows/make_portable.py
```

`MSYS2_ROOT` can select an existing project-local toolchain. The ordinary build
leaves `BB_P2P_DISTRIBUTION` off. With the flag on, the Windows launcher cannot
query or install stock upstream releases and the portable launcher keeps the
stable filename `CTR-NitroFueled-PC.exe`. The game-logo icon resource is excluded
from these executables; the launcher uses the system application icon. The complete distribution also supplies
`p2p-distribution.json` at its root; its presence disables stock update checks in
`run.py` and the GTK launcher. Updates must come from the complete distribution's
own release channel so the host and adapter remain compatible. No user settings
are changed to achieve this.

Runtime artifacts are `out/sacredfire.exe`, `out/bb-gpu-capabilities.exe`, and
`out/CTR-NitroFueled-PC.exe`. The optional relay backend and its dependency DLLs
are under `out/psn-bridge-tests/backends/`; use the documented backend configuration
from `platform/psn/backends/README.md`. The independent bridge sets `BB_PSN_BACKEND`
and the loopback relay configuration for the launched process. Source and binary
revisions must be recorded by the distribution pipeline.

When a backend is selected, platform identity, friends, and invitations are owned
by that backend. The upstream RyuNET friends worker and signed service requests
stay inactive; the overlay directs the player to the game's online friend list.
Without a selected backend, upstream nickname, friends, moderation, server theme,
and race countdown behavior remain unchanged.

Package only clean build artifacts and source-controlled runtime inputs. Existing
`user/`, saves, databases, personal configuration, game data, logs, and installed
profiles are never package inputs. Artwork in `art/` and `packaging/windows/steamgrid/`
has separate provenance and is not relicensed by this integration. The distribution
pipeline must decide which assets it can include and retain the applicable notices.
Contract tests establish adapter behavior only; they do not establish a live P2P
race or compatibility with every upstream game path.
