# librespot-cpp

A C++23 library port of librespot for Linux applications. The protocol reference is upstream commit `939dc5ee9d833e1980f9495241219d9d4868a061`.

The port is under development. The implemented components are usable independently, but this repository does **not yet provide a complete Spotify player or Spotify Connect receiver**.

## Build

Requirements:

- CMake 3.28 or newer and a compiler with C++23 support, including `std::expected`.
- Boost 1.81 or newer. Example applications additionally use Boost.Program_options.
- OpenSSL 3 or newer, development headers, and a system CA trust store.
- Git, Make, and NASM for the default FFmpeg source build.

CMake fetches pinned nlohmann JSON, Protobuf and its Abseil dependency, FFmpeg, and Catch2 sources from GitHub. The FFmpeg archive is checked against a SHA-256 digest. FFmpeg is built with just the required audio demuxers, decoders and parsers; NASM preserves its x86 assembly optimisations. Initial configuration requires network access.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure --parallel
```

The FFmpeg build defaults to the detected CPU count. Override it with `-DLIBRESPOT_DEPENDENCY_JOBS=N`. Use `-DLIBRESPOT_USE_SYSTEM_FFMPEG=ON` to link an installed FFmpeg 8 or newer instead; this requires `pkg-config` and the `libavformat`, `libavcodec`, and `libavutil` development packages.

For memory and undefined behaviour checks, configure a Debug build with `-DLIBRESPOT_ENABLE_SANITIZERS=ON`. This instruments the project and its CMake-built dependencies together, keeping Protobuf's container annotations consistent. FFmpeg's external source build retains its normal settings.

`-DBUILD_TESTING=OFF` omits Catch2 and tests. `-DLIBRESPOT_BUILD_EXAMPLES=OFF` omits example applications and the Program_options dependency.

The development build has been checked with GCC 16, Boost 1.90, and OpenSSL 3.6 on Linux. Older supported compiler and dependency combinations have not yet been exercised.

## Use from CMake

```cmake
add_subdirectory(path/to/librespot-cpp)
target_link_libraries(my_application PRIVATE librespot::librespot)
```

The library target propagates C++23 and its public include directories. Audio device integration belongs to the application; no ALSA, PulseAudio, PipeWire, or other device backend is required.

## Available components

| API | Behaviour |
| --- | --- |
| `spotify_id`, `file_id` | Checked raw, hexadecimal and base62 conversions with fixed protocol widths. |
| `parse_uri`, `spotify_uri` | Catalog, named playlist, local-file and unknown URI variants. Local-file fields retain their wire encoding. |
| `net::http_client` | Verified HTTPS on an application executor, bounded responses and DNS/network deadlines. |
| `oauth::device_auth` | Device pairing, individual token polls and refresh-token exchange through an injectable transport. |
| `oauth::service_auth` | Client-token and Login5 authentication with bounded hashcash challenges, token caching and refresh. |
| `service::client` | Service address resolution, authenticated track metadata, supported audio format selection and CDN storage resolution. |
| `net::access_point` | Address resolution, signed Diffie-Hellman handshake, authenticated Shannon packets and reusable-credential login. |
| `session` | An internal strand, bounded outgoing queue, concurrent Mercury/audio-key request dispatch, deadlines and delayed pong keepalive. |
| `net::encode_mercury`, `decode_mercury`, `mercury_assembler` | Mercury framing and bounded fragmented-response assembly. |
| `cache::save_credentials`, `load_credentials` | Atomic owner-only Linux credential persistence, using libvoxelstorm's base64 implementation with strict input validation. |
| `audio::fetch_range` | Validated HTTP range fetching with CDN failover, cancellation propagation and optional AES-CTR decryption. |
| `audio::cdn_source` | Seekable decoder input backed by one bounded CDN chunk, original-offset decryption and thread-safe cancellation. |
| `audio::decrypt` | In-place decryption at arbitrary encrypted-file offsets. |
| `audio::decoder`, `pcm_frame` | File or shared-memory FLAC, Vorbis, MP3 and PCM decoding, native precision and sample rate, and timestamped sample frames. |
| `audio::wav_writer` | Sample-preserving mono/stereo PCM or floating-point WAV output, including planar interleaving. |

`librespot/version.h` is generated from the single project version in `CMakeLists.txt`. The upstream reference revision is also available there.

## HTTP identity and asynchronous ownership

The library omits `User-Agent` by default. The owning program may configure one, and request headers can override it:

```cpp
librespot::net::http_client client{
  executor,
  {.user_agent{"my_application"}},
};
```

Asio's `awaitable` uses standard C++ coroutines and supplies their executor and networking integration. Request data is owned by the coroutine so temporary strings remain valid across suspension. Read-only synchronous interfaces use references or string views.

The transport, OAuth client, executor, and access-point objects must outlive outstanding operations. Access-point calls must execute on the same strand; at most one send and one receive may run concurrently. Closing an access point cancels socket operations; wait for their completion before destroying it.

`oauth::service_auth` takes a separate application-owned worker executor for hashcash computation. Keep that executor running and alive until authentication finishes. The example shares one worker between authentication challenges and PCM decoding; network operations remain on the HTTP executor. Service authentication and `service::client` calls should run sequentially on one network executor.

The OAuth API exposes individual polls so the application owns pairing presentation, cancellation and expiry. Respect the returned polling interval; a `slow_down` response increases it by five seconds.

## Example

```sh
./build/device_login --help
./build/device_login --client-id CLIENT_ID --user-agent my_application
```

Use a Spotify client ID enabled for device authorisation. The example prints pairing instructions and reports completion; it neither prints tokens nor writes credentials.

The PCM demonstration can be run offline:

```sh
./build/pcm_stream --input tests/fixtures/tone_stereo.ogg --output /tmp/tone.wav
```

It reports the first PCM frame and the total decoded frames, samples and checksum. WAV output is optional; omitting `--output` still consumes and checksums every frame.

To exercise the Spotify path:

```sh
./build/pcm_stream --track spotify:track:TRACK_ID \
  --client-id OAUTH_CLIENT_ID --credentials ./credentials.json \
  --output /tmp/track.wav
```

The credential file is optional. When specified, its parent directory must exist; the example saves reusable session credentials with owner-only permissions and reuses them on subsequent runs. The first run displays device-pairing instructions. Tokens and signed CDN URLs are never printed. Ctrl-C cancels the operation.

`--service-client-id` configures the Login5 protocol identity, which must match the reusable credentials. The default follows the pinned upstream Linux implementation. The client-token identity and Spotify protocol version are independently configurable through `service_auth_config`; these fields describe Spotify's wire protocol and do not set the HTTP user agent. Format selection prefers 24-bit FLAC, then 16-bit FLAC, then supported lossy formats. The example tries the next supported format when storage reports a restricted file. Alternative regional recordings are not resolved yet.

## PCM integration

`audio::decoder` performs synchronous work and should run on a worker thread. Each returned `pcm_frame` owns its sample buffers, including when the decoder advances or seeks. A plane view remains valid for the lifetime of its frame. Packed data has one plane; planar data has one plane per channel. `sample_count()` counts samples per channel. Integer samples use native byte order and signedness as described by `pcm_format`.

Frames preserve the codec's native sample representation. The application can choose mixing, normalisation, resampling, channel conversion and device output. Seeking discards decoder preroll before returning samples at or after the requested stream timestamp.

`audio::cdn_source` bridges asynchronous HTTPS and FFmpeg's synchronous input callbacks. Construct it with the HTTP executor, CDN URLs and an optional audio key, then pass its shared pointer to a decoder on a worker thread. The HTTP executor must keep running while the worker reads. The source retains one chunk (256 KiB by default); it requests further ranges on demand and refetches evicted chunks when seeking. There is no whole-track buffer or unbounded producer queue. Decoder probing can read ahead and seek before producing the first frame.

The source's `container_offset` is measured in the original encrypted file. Spotify Vorbis uses 167; FLAC and MP3 use zero. Decryption always uses the original file offset, before the container prefix is hidden from the decoder. `cancel()` is safe from another thread and interrupts an in-flight HTTP request. Keep the transport and executor alive until blocked reads have returned.

## Verification and remaining scope

Tests run offline against independent cipher/HMAC vectors, conversion vectors, generated compressed audio fixtures, a local TLS server, injected OAuth/CDN responses, and temporary credential files. They cover lossless FLAC sample accuracy, decoder seeking, retained frame ownership, malformed input and user-agent overrides.

Streaming tests verify that the first PCM frame precedes a complete download, compare every sample of encrypted input, exercise seek/cache eviction and cancellation, and round-trip mono/stereo WAV output without changing integer or floating-point sample values. Service tests exercise client-token and Login5 hashcash challenges, token caching and a rejected-token refresh, metadata identity checks, and restricted storage responses.

The Spotify authentication, metadata and streaming path has not yet been verified against a live Premium account. Album/artist/episode metadata, regional track alternatives, prefetch and persistent audio caching, player control/events, normalisation, discovery/zeroconf, and Spotify Connect state/control remain to be implemented. The presence of protocol schemas does not imply those features are implemented.

`session` serialises its internal work on its own strand and accepts independent concurrent requests. Packet callbacks execute on that strand and should return promptly. Its outgoing queue and pending request sets are bounded. A closed session must be recreated; automatic reconnection and resubscription are not implemented yet.

`session::request_token` accepts the application's client ID and a vector of requested scopes. It caches tokens separately for each client, reuses tokens covering a subset of their granted scopes, and refreshes on the next request within ten seconds of expiry. Tokens use a monotonic clock. HTTP user agents are omitted unless configured by the application; the example supplies its own name without a version suffix. The library version is defined once in `CMakeLists.txt` and exposed through the generated `librespot/version.h`.

## Licence

The C++ project uses the MIT licence in `LICENSE`. Upstream notices are retained in `LICENSES`; fetched dependencies retain their own licences.
