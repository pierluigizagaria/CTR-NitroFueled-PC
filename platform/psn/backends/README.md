# Local bridge backend

`psn-bridge-backend` connects the optional PSN host module to a separately
supervised platform provider. The provider maps its platform events into PSN
notification types and payloads. This adapter transports requests and copied
notifications, while the host owns guest memory, filters, callbacks and
SystemService events. No Steam library, game packet parser or provider source is
linked into the host.

The backend is opt-in. The normal host build and built-in service path remain the
default. A supervisor such as [ctrnf-p2p](https://github.com/pierluigizagaria/ctrnf-p2p)
starts the selected host with:

```text
BB_PSN_BACKEND=<absolute path to psn-bridge-backend.dll or .so>
BB_PSN_BACKEND_CONFIG={"url":"http://127.0.0.2:31380","token":"<64 lowercase hex characters>"}
```

The supervisor generates a fresh capability for each session and provides the
configured publisher route separately. The capability belongs only in the child
environment; do not save it in launcher preferences, logs or a reusable config.
Local processes with access to that environment share the same trust boundary.
Startup failure does not fall back to another platform account or backend.

## Build

Requires an x86-64 C11/C++20 toolchain, CMake 3.24, threads, libcurl 7.85 or later,
and nlohmann JSON 3.11 or later. These dependencies are only needed when this
target is enabled; there is no dependency on the renderer or sibling repositories.

```sh
cmake -S platform/psn -B out/psn-bridge -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPSN_BRIDGE_BACKEND=ON
cmake --build out/psn-bridge
ctest --test-dir out/psn-bridge --output-on-failure
```

Use `CMAKE_PREFIX_PATH` for locally retained dependencies, or
`PSN_JSON_INCLUDE_DIR` for a header-only JSON installation. On Windows the build
copies the module's non-system runtime dependencies beside it in the build
directory. Keep those DLLs together when deploying the module; no system-wide
installation is required. A distributable package must preserve the licenses of
its actual libcurl, compiler-runtime and transitive dependency builds. This build
step does not publish a package or bundle any Valve files.

## Relay contract

Only numeric IPv4 loopback URLs (`127.0.0.0/8`) with an explicit port are accepted.
Every HTTP request sends `Authorization: Bearer <capability>`. Proxies, redirects,
URL credentials, DNS names, remote hosts and URL paths in the endpoint are refused.

The version-1 operations are:

| Operation | Purpose |
|---|---|
| `GET /psn/v1/info` | Authenticated identity and capability handshake |
| `POST /psn/v1/token` | Asynchronous local publisher credential request |
| `POST /psn/v1/webapi` | Copied method, group, path/query, headers and binary body |
| `GET /psn/v1/events?after=N` | Sequenced PSN notifications and accepted invitations |

Account IDs are canonical decimal strings, never JSON floating-point values.
WebAPI bodies use lowercase hexadecimal and are bounded to 256 KiB in each
direction; metadata is bounded to 64 KiB and responses to 1 MiB. Duplicate
case-insensitive header names are unsupported because the relay's header object
cannot preserve them. HTTP error status and transport failure remain distinct.

The handshake requires identity, ID-token, WebAPI, push and accepted-invitation
capabilities, version 1 and a signed-in identity. One local PS4 user maps to that
account. The optional `local_user_id` configuration field selects its opaque
nonnegative ID (default `1`). The integrating host must
include that ID in its local-user snapshot; this selection does not change the
platform account or relay protocol. Identity, requests, subscriptions and accepted
invitation events preserve the selected local ID. The session cannot switch accounts.
The fixed local adult policy
(age 99, Plus-authorized, unrestricted) preserves the port's local policy; it is
not a claim about a PSN subscription or Steam parental enforcement. Title and
content-policy inputs are validated locally. Sony application secrets are never
forwarded or logged. Token issuer 256 describes the local compatibility adapter,
not Sony issuance. Authorization-code requests and invitation dialog UI return
an explicit unsupported result. A guest SEND request with exactly one fixed
recipient is supported without opening a picker, as described below.

## Lifetime and failure

Only startup performs a bounded synchronous handshake. Auth/WebAPI starts copy
their inputs and return handles immediately. A request worker performs HTTP,
with a 30-second transport budget; cancellation interrupts an active transfer and
release waits for its work to quiesce. Token and WebAPI requests are never retried
after an uncertain result. A separate event worker keeps notifications moving
during a slow request. Close stops and joins both workers before module unload.

The event cursor advances only after validating and copying a batch. Replayed
sequence numbers do not produce duplicate invitations; gaps or obsolete cursors
fail the session. Each matching registration receives its own cookie. A null
service differs from an empty service name, and only named services match a
service label. Unsubscribe removes queued events for that cookie. Guest callbacks
run only when the guest pumps its NP callbacks, never on either worker.

The adapter retains at most 128 requests, 512 subscriptions and 1024 queued
notifications. Overflow, malformed events and account changes fail explicitly;
transient event transport errors retry the same cursor at most five times.
There is no idle or session-duration timeout. Invitation IDs exceeding the PSN
guest field's 60-byte limit are rejected rather than truncated. An accepted
invitation preserves the original session, invitation and referring account IDs;
it does not commit game membership on the game's behalf.

The HTTP fixture exercises the real module, exact account IDs, binary requests,
six-second replies, active cancellation, subscription fanout, guest callback
dispatch and self-unregister, accepted-invitation layout and cursor replay,
malformed/account-change/overflow failures, credentials, redirects and endpoint
validation, and explicit local-user mapping with IDs `70001` and `0`. These are
offline contract checks. They do not establish Steam
delivery, successful in-game joining or friend-page refresh in a live game.

## Selected-friend invitations

The guest InvitationDialog SEND action may supply exactly one selected account
(`OpenA`) or legacy online ID (`Open`) and a canonical decimal session ID. The
backend copies these inputs and returns an asynchronous handle. Through the
existing WebAPI relay it reads `GET /v1/users/me/friendList`, requires one exact
account match or one unambiguous legacy-name match, then submits
`POST /v1/sessions/{sessionId}/invitations` with `{"to":["accountId"]}`. The
provider owns friendship, presence, session namespace and membership validation,
and performs the actual platform invitation. No Steam symbols or provider source
are linked into the host; no new relay endpoint or ABI is required.

Only provider success with HTTP 204 completes the guest result successfully and
returns the resolved recipient. This confirms dispatch, not remote delivery or
acceptance. Missing/ambiguous recipients, incomplete snapshots, provider/HTTP
errors and lost responses complete with an error and no confirmed recipient.
The mutation is never automatically retried. Cancel before mutation suppresses
the send; cancel after dispatch cannot retract an invitation already issued by
the provider. Release waits until the worker stops using the request.

Empty selection/picker requests, receive dialogs, multiple recipients and custom
messages remain explicitly unsupported. No dialog selection or successful send
is fabricated from opening or closing the Steam overlay. Fixed-recipient fixture
tests cover copied inputs, both API identity forms, guest OpenA/results, numeric
names, absent/ambiguous recipients, failures, active cancellation and a lost
mutation response. These checks do not establish live platform delivery.

## Optional timing diagnostics

Set `PSN_BRIDGE_TRACE_TIMING=1` in the environment before starting the
supervised session to emit timing records to the host's stderr log. Only the
exact value `1` enables this mode; it is off by default. No configuration or
protocol fields change.

The records contain monotonic milliseconds since backend initialization and
fixed operation categories. They never contain paths, queries, account or
session IDs, names, authorization headers, tokens or request/response bodies.
Only accepted invitations and WebAPI completions are logged:

- `invitation_received`: the backend validated and queued an accepted
  invitation from the relay; `fetch_ms` covers that local HTTP event read.
- `invitation_to_host`: `event_next` returned the event to the host;
  `queue_ms` measures its wait inside this backend.
- `web_complete`: the host first consumed a completed WebAPI result.
  `queue_ms` covers waiting for the request worker, `service_ms` covers the
  request work until completion or cancellation, and `consume_ms` covers
  the retained result's wait for its first poll. Status and HTTP code are numeric.
  Repeated polls of the same result do not duplicate this record.

These measurements do not show Steam's callback, lobby validation or session
blob transfer before the accepted event reaches the relay. Delivery to the
host is also distinct from the game's later SystemService event consumption
and actual lobby entry. The WebAPI service duration includes the relay/provider
work without attributing it to an internal phase. This diagnostic does not
change polling intervals, joins or timeouts, and does not establish gameplay
latency by itself.
