# Dual-Protocol HTTP Client for Seastar — Design Document

## 1. Problem Statement

The current Seastar HTTP client (`seastar::http::experimental::client`) only supports HTTP/1.1.
HTTP/2 offers stream multiplexing over a single TCP connection, header compression (HPACK),
and binary framing — all of which reduce latency and improve throughput for workloads that
make many concurrent requests to the same host (the dpu-proxy bidding use case).

We need a **single client class** that supports **both HTTP/2 and HTTP/1.1**, with transparent
protocol negotiation via TLS ALPN or explicit configuration for cleartext.

## 2. Existing Architecture Summary

```
┌─────────────────────────────────────────────────────────────┐
│  client                                                     │
│  - owns connection_factory                                  │
│  - manages connection pool (boost::intrusive::list)         │
│  - max_connections limit                                    │
│  - retry logic                                              │
│  - with_connection() / with_new_connection() helpers        │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  connection (1 per TCP socket)                       │   │
│  │  - connected_socket _fd                              │   │
│  │  - input_stream<char> _read_buf                      │   │
│  │  - output_stream<char> _write_buf                    │   │
│  │  - HTTP/1.1 text framing (request_line, headers)     │   │
│  │  - one in-flight request at a time                   │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  connection_factory ─► connected_socket                     │
│    ├─ basic_connection_factory (TCP)                        │
│    └─ tls_connection_factory   (TLS, no ALPN today)        │
│                                                             │
│  Types reused from http/request.hh and http/reply.hh       │
│    request { _method, _url, _headers, content_length,      │
│              body_writer, content }                         │
│    reply   { _status, _headers, content_length,            │
│              consumed_content }                             │
└─────────────────────────────────────────────────────────────┘
```

Key **constraints** in the current design:
- `connection` is strictly 1-request-at-a-time; the pool gives concurrency by opening N connections.
- `client::with_connection()` borrows a `connection` from the pool, runs the handler, returns it.
- `request.request_line()` and `request.write_request_headers()` produce HTTP/1.1 text.
- `recv_reply()` uses a Ragel-generated `http_response_parser` (HTTP/1.1 text).
- `connection_factory` creates raw `connected_socket`; does not negotiate ALPN.

## 3. Design Goals

1. **Single client class** — supports both HTTP/2 and HTTP/1.1 transparently.
2. **Protocol negotiation** — ALPN over TLS; explicit configuration for cleartext.
3. **Same API** — `make_request()` signature identical to the existing client.
4. **Reuse `http::request` and `http::reply`** — callers construct requests the same way.
5. **Integrate with Seastar's event loop** (no threads) — use `nghttp2` in non-blocking callback mode.
6. **Keep existing HTTP/1.1 client code untouched** — h2_client delegates to it for h1 mode.
7. **Backward compatible** — works with existing `connection_factory` via adapter.

## 4. Dependency: nghttp2

nghttp2 is the de-facto C library for HTTP/2 framing. It provides:
- `nghttp2_session` — state machine for connection-level h2 processing.
- Callback-driven: you feed it raw bytes and it calls your frame/data/header callbacks.
- No I/O of its own — fits perfectly with Seastar's async model.

**Integration model**: Seastar drives the socket I/O; nghttp2 drives the h2 state machine.

## 5. Class Hierarchy

```
seastar::http::experimental
├── http_protocol enum          (new: unknown, http_1_1, h2)
├── protocol_preference enum    (new: negotiate, h2_only, h1_only, h2_with_h1_fallback)
│
├── connection_factory          (existing, reused)
│   ├── basic_connection_factory
│   └── tls_connection_factory
│
├── protocol_connection_factory (new: returns socket + negotiated protocol)
│   ├── basic_protocol_connection_factory  (cleartext, explicit protocol)
│   ├── alpn_tls_connection_factory        (TLS with ALPN negotiation)
│   └── legacy_factory_adapter             (wraps old connection_factory)
│
├── client                      (existing HTTP/1.1, unchanged)
├── connection                  (existing HTTP/1.1, unchanged)
│
└── h2/                         (new namespace)
    ├── h2_stream               Per-stream request/response state
    ├── h2_connection           Stream-multiplexing connection
    └── h2_client               Dual-protocol client (h1 + h2)
```

## 6. Detailed Class Design

### 6.1 Protocol Types

```cpp
/// The HTTP protocol version negotiated or configured for a connection
enum class http_protocol : uint8_t {
    unknown,    // Not yet determined (pre-negotiation)
    http_1_1,   // HTTP/1.1
    h2,         // HTTP/2
};

/// Preferred protocol selection strategy
enum class protocol_preference : uint8_t {
    negotiate,            // ALPN negotiation; fallback to h1 (default for TLS)
    h2_only,              // Require HTTP/2
    h1_only,              // Use HTTP/1.1 only
    h2_with_h1_fallback,  // Prefer h2, fall back to h1
};
```

### 6.2 Protocol-Aware Connection Factory

The key abstraction that enables dual-protocol support. Returns both a socket
and the negotiated protocol:

```cpp
struct negotiated_connection {
    connected_socket socket;
    http_protocol protocol = http_protocol::unknown;
};

class protocol_connection_factory {
public:
    virtual future<negotiated_connection> make(abort_source*) = 0;
    virtual ~protocol_connection_factory() {}
};
```

**Implementations:**

| Factory | Transport | Protocol Selection |
|---------|-----------|-------------------|
| `basic_protocol_connection_factory` | TCP | Explicit (h1 or h2c) |
| `alpn_tls_connection_factory` | TLS | ALPN negotiation |
| `legacy_factory_adapter` | Any | Wraps old `connection_factory` with default protocol |

### 6.3 TLS ALPN Extension

```cpp
// net/tls.hh — extend tls_options
struct tls_options {
    // ... existing fields ...

    /// ALPN protocol list for negotiation, e.g. {"h2", "http/1.1"}
    std::vector<sstring> alpn_protocols = {};
};
```

### 6.4 h2_stream

Represents a single HTTP/2 stream (one request-response pair).
See `h2/h2_stream.hh` for full definition.

### 6.5 h2_connection

Manages a single TCP connection with many concurrent streams via nghttp2.
See `h2/h2_connection.hh` for full definition.

### 6.6 h2_client — Dual-Protocol Client

The main client class supporting both HTTP/2 and HTTP/1.1:

```cpp
class h2_client {
public:
    using reply_handler = noncopyable_function<future<>(const reply&, input_stream<char>&& body)>;
    using retry_requests = bool_class<struct h2_retry_requests_tag>;

    // --- Constructors ---

    // Cleartext: explicit protocol (default h1)
    explicit h2_client(socket_address addr,
                       protocol_preference pref = protocol_preference::h1_only,
                       unsigned max_connections = 0);

    // TLS: ALPN negotiation (default: negotiate)
    h2_client(socket_address addr,
              shared_ptr<tls::certificate_credentials> creds,
              sstring host,
              protocol_preference pref = protocol_preference::negotiate,
              unsigned max_connections = 0);

    // Protocol-aware factory
    explicit h2_client(std::unique_ptr<protocol_connection_factory> f,
                       unsigned max_connections = 0,
                       retry_requests retry = retry_requests::no);

    // Legacy factory (backward compatible)
    explicit h2_client(std::unique_ptr<connection_factory> f,
                       http_protocol default_proto = http_protocol::http_1_1,
                       unsigned max_connections = 0,
                       retry_requests retry = retry_requests::no);

    // --- API (identical to existing client) ---

    future<> make_request(request&& req, reply_handler&& handle,
                          std::optional<reply::status_type>&& expected = std::nullopt,
                          abort_source* as = nullptr);

    future<> make_request(request& req, reply_handler& handle,
                          std::optional<reply::status_type> expected = std::nullopt,
                          abort_source* as = nullptr);

    future<> close();
    future<> set_maximum_connections(unsigned nr);

    http_protocol active_protocol() const noexcept;
    unsigned connections_nr() const noexcept;
    unsigned long total_new_connections_nr() const noexcept;
    size_t total_active_streams() const noexcept;  // 0 for h1

private:
    // Protocol is determined on first connection and fixed thereafter
    http_protocol _active_protocol = http_protocol::unknown;
    protocol_preference _preference;

    // h1 backend: delegates to existing seastar::http::experimental::client
    std::unique_ptr<client> _h1_client;

    // h2 backend: manages h2_connections directly
    std::vector<shared_ptr<h2_connection>> _h2_connections;
    unsigned _h2_nr_connections = 0;
    condition_variable _h2_wait_stream;
};
```

**Protocol determination flow:**

```
make_request() called
    │
    ├─ _active_protocol == unknown?
    │   │
    │   yes─► ensure_protocol_determined()
    │   │     ├─ factory->make()  → negotiated_connection
    │   │     ├─ set _active_protocol from negotiated result
    │   │     └─ initialize appropriate backend (h1 or h2)
    │   │         with the already-established connection
    │   │
    │   no─►─┐
    │        │
    ├────────┘
    │
    ├─ _active_protocol == h2?
    │   └─► do_make_request_h2(...)
    │       └─ find/create h2_connection with available stream slot
    │       └─ submit_request(), get_reply(), handle response
    │
    └─ _active_protocol == http_1_1?
        └─► do_make_request_h1(...)
            └─ delegate to _h1_client->make_request()
```

## 7. Data Flow

### 7.1 Protocol Negotiation (TLS with ALPN)

```
h2_client                   alpn_tls_connection_factory        GnuTLS          Server
    │                              │                              │               │
    │─ensure_protocol_determined()─►│                              │               │
    │                              │─tls::connect(alpn={"h2","http/1.1"})────────►│
    │                              │                              │◄──TLS hello──│
    │                              │                              │──ALPN: "h2"─►│
    │                              │◄─negotiated_connection{sock, h2}─────────────│
    │◄─_active_protocol = h2───────│                              │               │
    │                              │                              │               │
    │  [all subsequent requests    │                              │               │
    │   now use h2 path]           │                              │               │
```

### 7.2 Dual-Protocol Request Routing

```
make_request()
    │
    ├─ active_protocol == unknown
    │   └─ ensure_protocol_determined()
    │       └─ factory->make() → {socket, protocol}
    │       └─ initialize h1_client or h2_connections
    │
    ├─ active_protocol == h2
    │   └─ do_make_request_h2()
    │       ├─ get_h2_connection() → find one with stream capacity
    │       ├─ con.submit_request(req)
    │       │   ├─ nghttp2_submit_request() → HEADERS frame
    │       │   └─ send_loop() → wire
    │       ├─ stream->get_reply() → wait for response HEADERS
    │       ├─ handle(reply, body_stream)
    │       └─ stream cleanup
    │
    └─ active_protocol == http_1_1
        └─ do_make_request_h1()
            └─ _h1_client->make_request(req, handle, expected, as)
                └─ [existing HTTP/1.1 pool logic]
```

### 7.3 HTTP/2 Request Lifecycle

```
User                h2_client          h2_connection           nghttp2              socket
  │                    │                    │                     │                    │
  │─make_request()────►│                    │                     │                    │
  │                    │─get_h2_connection()►│                     │                    │
  │                    │                    │                     │                    │
  │                    │                    │─submit_request()───►│                    │
  │                    │                    │  nghttp2_submit_    │                    │
  │                    │                    │  request()          │                    │
  │                    │                    │                     │                    │
  │                    │                    │─send_loop()─────────┼──write HEADERS────►│
  │                    │                    │                     │  write DATA ──────►│
  │                    │                    │                     │                    │
  │                    │                    │◄────────────────────┼──read HEADERS──────│
  │                    │                    │  on_header cb       │                    │
  │                    │                    │  on_frame_recv cb   │                    │
  │                    │                    │                     │                    │
  │                    │                    │◄────────────────────┼──read DATA─────────│
  │                    │                    │  on_data_chunk_recv │                    │
  │                    │                    │  ──►pipe write      │                    │
  │                    │                    │                     │                    │
  │◄──reply_handler()──│◄──future<reply>───│                     │                    │
  │  read body via     │                    │                     │                    │
  │  input_stream      │                    │                     │                    │
```

### 7.4 Multiplexing Model (h2 vs h1)

```
HTTP/2 mode:
                       h2_connection
                      ┌─────────────────────────────────────┐
                      │  nghttp2_session                    │
    socket ◄─────────►│     ┌─────────────────────────────┐ │
    (single TCP)      │     │ stream 1 (req A) ─► reply A │ │
                      │     │ stream 3 (req B) ─► reply B │ │
                      │     │ stream 5 (req C) ─► reply C │ │
                      │     │ ...                         │ │
                      │     └─────────────────────────────┘ │
                      └─────────────────────────────────────┘

HTTP/1.1 mode (delegated to existing client):

    connection 1 ◄──► req A ──► reply A
    connection 2 ◄──► req B ──► reply B
    connection 3 ◄──► req C ──► reply C
```

## 8. Implementation Details

### 8.1 I/O Loop Architecture

The `h2_connection` runs two concurrent fibers:

**read_loop()** — continuously reads from the socket and feeds data to nghttp2:
```cpp
future<> h2_connection::read_loop() {
    while (!_goaway_received && !_read_buf.eof()) {
        auto buf = co_await _read_buf.read();
        if (buf.empty()) break;

        int rv = nghttp2_session_mem_recv(_session,
            reinterpret_cast<const uint8_t*>(buf.get()), buf.size());
        if (rv < 0) {
            // protocol error → send GOAWAY
            co_await send_goaway(rv);
            break;
        }
        // nghttp2 may have produced response frames (WINDOW_UPDATE, etc.)
        _send_pending.signal(1);
    }
}
```

**send_loop()** — drains nghttp2's output buffer to the socket:
```cpp
future<> h2_connection::send_loop() {
    while (!_goaway_sent) {
        co_await _send_pending.wait();

        const uint8_t* data;
        ssize_t len;
        while ((len = nghttp2_session_mem_send(_session, &data)) > 0) {
            co_await _write_buf.write(reinterpret_cast<const char*>(data), len);
        }
        co_await _write_buf.flush();
    }
}
```

### 8.2 Request Body Streaming

HTTP/2 DATA frames are sent via nghttp2's data provider callback.
The `on_data_source_read` callback reads from the request's body:

```cpp
// Adapter: wraps request.body_writer into an nghttp2 data provider
struct request_body_provider {
    pipe<temporary_buffer<char>> body_pipe;
    pipe_reader<temporary_buffer<char>> reader;
    bool eof = false;

    // Called by nghttp2 when it wants to send DATA
    static ssize_t read_callback(nghttp2_session*, int32_t stream_id,
                                 uint8_t* buf, size_t length,
                                 uint32_t* data_flags,
                                 nghttp2_data_source* source,
                                 void* user_data) {
        auto* prov = static_cast<request_body_provider*>(source->ptr);

        if (prov->eof) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }

        // If data is available synchronously, copy and return
        // Otherwise return NGHTTP2_ERR_DEFERRED and resume later
        // when the body_writer pipe has data
        ...
    }
};
```

The deferred-data pattern is key: when the request body is not yet available,
`on_data_source_read` returns `NGHTTP2_ERR_DEFERRED`. When body data arrives
(from `req.body_writer` writing into a pipe), we call
`nghttp2_session_resume_data(session, stream_id)` and signal the send loop.

### 8.3 Response Body Delivery

Response DATA frames arrive in `on_data_chunk_recv`. The data is pushed into
the stream's body pipe, which backs the `input_stream<char>` returned to the caller:

```cpp
int h2_connection::on_data_chunk_recv(nghttp2_session*, uint8_t flags,
                                       int32_t stream_id,
                                       const uint8_t* data, size_t len,
                                       void* user_data) {
    auto* conn = static_cast<h2_connection*>(user_data);
    auto it = conn->_streams.find(stream_id);
    if (it == conn->_streams.end()) return 0;

    auto& stream = it->second;
    // Push data into the pipe; the consumer reads via input_stream
    auto buf = temporary_buffer<char>(reinterpret_cast<const char*>(data), len);
    stream->_body_pipe.writer.write(std::move(buf));
    // (in practice, manage backpressure via WINDOW_UPDATE)
    return 0;
}
```

### 8.4 Header Mapping: h2 ↔ Seastar Types

HTTP/2 uses pseudo-headers (`:method`, `:path`, `:scheme`, `:authority`, `:status`).
These map to existing `request`/`reply` fields:

| h2 Pseudo-Header | Seastar Type Field        |
|-------------------|---------------------------|
| `:method`         | `request._method`         |
| `:path`           | `request._url`            |
| `:scheme`         | `request.protocol_name`   |
| `:authority`      | `request._headers["Host"]`|
| `:status`         | `reply._status`           |

Regular headers map 1:1 to `_headers` map (lowercased per h2 spec).

### 8.5 Connection Lifecycle

```
make_connection()
    │
    ├── connection_factory::make()  → connected_socket
    │
    ├── h2_connection(fd)
    │
    ├── initialize()
    │   ├── nghttp2_session_client_new()
    │   ├── nghttp2_submit_settings()
    │   ├── write connection preface (PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n)
    │   └── start read_loop() + send_loop() as background fibers
    │
    ├── [submit_request() × N concurrently]
    │
    └── close()
        ├── nghttp2_submit_goaway()
        ├── drain pending streams
        ├── _write_buf.close(), _read_buf.close()
        └── nghttp2_session_del()
```

## 9. Flow Control

HTTP/2 has two levels of flow control:
- **Connection-level**: shared window across all streams
- **Stream-level**: per-stream window

nghttp2 manages flow control internally. The key integration points:

1. **Receiving data**: After consuming DATA in `on_data_chunk_recv`, we should call
   `nghttp2_session_consume(session, stream_id, len)` to acknowledge consumption and
   trigger WINDOW_UPDATE frames.

2. **Sending data**: `on_data_source_read` should respect the available window.
   nghttp2 will only call it when window space is available.

3. **Backpressure**: If the consumer of the response body is slow, we stop calling
   `nghttp2_session_consume()`, which prevents WINDOW_UPDATE and applies backpressure
   to the server. This maps naturally to Seastar's pipe backpressure.

## 10. Error Handling

| Error Type | h2 Mechanism | Seastar Surface |
|------------|-------------|-----------------|
| Stream error | RST_STREAM | Exception on that stream's future |
| Connection error | GOAWAY | Exception on all pending streams |
| Protocol error | GOAWAY + close | Exception + connection teardown |
| Timeout | abort_source | nghttp2_submit_rst_stream per stream |

`is_retryable_exception()` can be extended to recognize h2-specific errors for retry logic.

## 11. Changes Required to Existing Seastar Code

### 11.1 net/tls.hh — Add ALPN to tls_options

```diff
 struct tls_options {
     bool wait_for_eof_on_shutdown = true;
     sstring server_name = {};
     bool verify_certificate = true;
     session_data session_resume_data;
+    /// ALPN protocol list for TLS negotiation (e.g., {"h2", "http/1.1"})
+    std::vector<sstring> alpn_protocols = {};
 };
```

### 11.2 net/tls.cc — Wire ALPN into GnuTLS

```cpp
// In the TLS session setup, after setting SNI:
if (!options.alpn_protocols.empty()) {
    std::vector<gnutls_datum_t> protos;
    for (auto& p : options.alpn_protocols) {
        protos.push_back({
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(p.data())),
            static_cast<unsigned>(p.size())
        });
    }
    gnutls_alpn_set_protocols(session, protos.data(), protos.size(), 0);
}
```

After handshake, the negotiated protocol can be retrieved:
```cpp
gnutls_datum_t proto;
gnutls_alpn_get_selected_protocol(session, &proto);
// → "h2" or "http/1.1"
```

### 11.3 net/tls.hh — Expose negotiated ALPN on connected_socket (optional)

For `alpn_tls_connection_factory` to report the negotiated protocol, we either:
- (a) Query GnuTLS session after handshake inside the factory implementation, or
- (b) Add a method to `connected_socket` to retrieve the negotiated ALPN string

Option (a) is simpler and does not change the `connected_socket` API.

### 11.4 New Files

| File | Purpose |
|------|---------|
| `include/seastar/http/h2/h2_stream.hh` | `h2_stream` class |
| `include/seastar/http/h2/h2_connection.hh` | `h2_connection` class |
| `include/seastar/http/h2/h2_client.hh` | Dual-protocol `h2_client` + factory types |
| `src/http/h2/h2_connection.cc` | Connection implementation |
| `src/http/h2/h2_client.cc` | Client + factory implementation |

### 11.5 Build System (meson.build / CMakeLists.txt)

Add `nghttp2` as optional dependency:
```meson
nghttp2_dep = dependency('libnghttp2', required: get_option('http2'))
if nghttp2_dep.found()
    src += files('src/http/h2/h2_connection.cc', 'src/http/h2/h2_client.cc')
    deps += nghttp2_dep
    config.set('SEASTAR_HTTP2', true)
endif
```

## 12. API Compatibility — dpu-proxy Impact

The `h2_client` exposes the **same `make_request()` signature** as `http::experimental::client`.
The dpu-proxy `HttpClientService` can switch with minimal changes:

```diff
-using Client = seastar::http::experimental::client;
+using Client = seastar::http::experimental::h2::h2_client;
```

Factory construction changes (TLS with ALPN negotiation):
```diff
-factory = std::make_unique<seastar::http::experimental::basic_connection_factory>(addr);
-it = m_impl->clients.try_emplace(job.connectionInfo,
-    std::move(factory), m_impl->maxConnectionsPerHost).first;
+auto pref = config.prefer_h2
+    ? seastar::http::experimental::protocol_preference::h2_with_h1_fallback
+    : seastar::http::experimental::protocol_preference::h1_only;
+it = m_impl->clients.try_emplace(job.connectionInfo,
+    addr, creds, host, pref).first;
```

For cleartext with explicit h2c:
```diff
+using Client = seastar::http::experimental::h2::h2_client;
+Client client(addr, seastar::http::experimental::protocol_preference::h2_only);
```

The `reply_handler` callback remains identical — it receives `const reply&` and `input_stream<char>&& body`.

The client can also be used in h1-only mode as a drop-in replacement:
```cpp
// Behaves identically to the existing client (HTTP/1.1 only)
h2_client client(addr, protocol_preference::h1_only, /*max_connections=*/100);
```

## 13. Protocol Selection Matrix

| Constructor | Transport | ALPN | Protocol Result |
|-------------|-----------|------|-----------------|
| `h2_client(addr)` | TCP | N/A | HTTP/1.1 |
| `h2_client(addr, h2_only)` | TCP | N/A | h2c (prior knowledge) |
| `h2_client(addr, creds, host)` | TLS | {"h2","http/1.1"} | Server's choice |
| `h2_client(addr, creds, host, h2_only)` | TLS | {"h2"} | h2 (fail if unsupported) |
| `h2_client(addr, creds, host, h1_only)` | TLS | none | HTTP/1.1 |
| `h2_client(legacy_factory, h2)` | Any | N/A | h2 (prior knowledge) |
| `h2_client(legacy_factory, h1_1)` | Any | N/A | HTTP/1.1 |

## 14. Testing Strategy

| Test | Description |
|------|-------------|
| Unit: h2 frame encoding | Validate nghttp2 integration with mock socket |
| Unit: stream multiplexing | Submit N requests, verify concurrent resolution |
| Unit: flow control | Verify backpressure propagates via WINDOW_UPDATE |
| Unit: GOAWAY handling | Server sends GOAWAY, verify graceful shutdown |
| Unit: h1 fallback | ALPN returns http/1.1, verify client uses h1 path |
| Unit: protocol locking | After first connection, verify protocol stays fixed |
| Integration: against nginx | Dual-protocol client → nginx with h2 enabled |
| Integration: h2c cleartext | h2_client(addr, h2_only) → h2c-capable server |
| Integration: dpu-proxy e2e | Full bidding pipeline with h2 backend |

## 15. Performance Considerations

1. **Single connection** is usually sufficient for h2 — multiplexes hundreds of streams.
   Default `max_connections = 1` for h2 mode, `100` for h1 mode.

2. **HPACK compression** reduces header overhead significantly for repeated requests
   to the same host (all requests share the HPACK dynamic table on the connection).

3. **Avoid copies** in `on_data_chunk_recv` — use `temporary_buffer::share()` where possible.

4. **Send coalescing** — batch multiple frames into a single `_write_buf.write()` call
   via `nghttp2_session_mem_send()` which returns all pending data at once.

5. **Connection-level parallelism** — for extremely high throughput, allow 2-4 h2 connections
   to avoid head-of-line blocking at the TCP layer.

6. **h1 mode has zero overhead** — when h1 is selected, all requests delegate directly
   to the existing `seastar::http::experimental::client` with no extra indirection.

## 16. Implementation Phases

| Phase | Scope | Effort |
|-------|-------|--------|
| 1 | ALPN support in `tls_options` + GnuTLS wiring | 1 day |
| 2 | `protocol_connection_factory` + `alpn_tls_connection_factory` | 1 day |
| 3 | `h2_connection` + `h2_stream` with nghttp2 | 3-4 days |
| 4 | `h2_client` dual-protocol routing + h1 delegation | 1-2 days |
| 5 | Integration tests (h2 against nginx, h1 fallback) | 1 day |
| 6 | dpu-proxy integration + benchmarking | 1-2 days |

**Total: ~8-11 days**
