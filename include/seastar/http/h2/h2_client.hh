/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <boost/intrusive/list.hpp>
#endif
#include <seastar/core/abort_source.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/http/client.hh>
#include <seastar/http/connection_factory.hh>
#include <seastar/http/h2/h2_connection.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/request.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/bool_class.hh>
#include <seastar/util/noncopyable_function.hh>

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace seastar::http::experimental {

/// The HTTP protocol version negotiated or configured for a connection
enum class http_protocol : uint8_t {
    unknown,    ///< Not yet determined (pre-negotiation)
    http_1_1,   ///< HTTP/1.1
    h2,         ///< HTTP/2
};

/// Preferred protocol selection strategy
enum class protocol_preference : uint8_t {
    negotiate,  ///< Use ALPN to negotiate; fallback to h1 if no h2 (default for TLS)
    h2_only,    ///< Require HTTP/2 (h2 over TLS, h2c over cleartext with prior knowledge)
    h1_only,    ///< Use HTTP/1.1 only (same behavior as existing client)
    h2_with_h1_fallback, ///< Prefer h2, fall back to h1 if server doesn't support it
};

/**
 * \brief Connection factory that reports the negotiated HTTP protocol
 *
 * Extends the base connection_factory to additionally report which HTTP protocol
 * was negotiated during TLS handshake (via ALPN) or configured for cleartext.
 *
 * Implementations should set the protocol field in the returned negotiated_connection.
 */
struct negotiated_connection {
    connected_socket socket;
    http_protocol protocol = http_protocol::unknown;
};

class protocol_connection_factory {
public:
    virtual future<negotiated_connection> make(abort_source*) = 0;
    virtual ~protocol_connection_factory() {}
};

/**
 * \brief Plain TCP factory with explicit protocol selection (for h2c or h1)
 *
 * For h2c (cleartext HTTP/2), set protocol to http_protocol::h2.
 * For HTTP/1.1 cleartext, set protocol to http_protocol::http_1_1.
 */
class basic_protocol_connection_factory : public protocol_connection_factory {
    socket_address _addr;
    http_protocol _protocol;
public:
    explicit basic_protocol_connection_factory(socket_address addr,
                                               http_protocol proto = http_protocol::http_1_1)
        : _addr(std::move(addr))
        , _protocol(proto)
    {
    }
    future<negotiated_connection> make(abort_source* as) override {
        auto cs = co_await seastar::connect(_addr, {}, transport::TCP);
        co_return negotiated_connection{std::move(cs), _protocol};
    }
};

/**
 * \brief TLS factory with ALPN protocol negotiation
 *
 * Connects via TLS and uses ALPN to negotiate between "h2" and "http/1.1".
 * The negotiated protocol is reported in the returned negotiated_connection.
 *
 * ALPN protocol list is determined by the protocol_preference:
 *  - negotiate / h2_with_h1_fallback: ALPN = {"h2", "http/1.1"}
 *  - h2_only: ALPN = {"h2"}
 *  - h1_only: no ALPN (or ALPN = {"http/1.1"})
 */
class alpn_tls_connection_factory : public protocol_connection_factory {
    socket_address _addr;
    shared_ptr<tls::certificate_credentials> _creds;
    sstring _host;
    protocol_preference _preference;
public:
    alpn_tls_connection_factory(socket_address addr,
                                shared_ptr<tls::certificate_credentials> creds,
                                sstring host,
                                protocol_preference pref = protocol_preference::negotiate)
        : _addr(std::move(addr))
        , _creds(std::move(creds))
        , _host(std::move(host))
        , _preference(pref)
    {
    }

    /// The returned negotiated_connection will have the protocol field populated
    /// based on the ALPN negotiation result.
    ///
    /// If ALPN negotiation fails (server doesn't support ALPN), the behavior depends
    /// on preference:
    ///   - h2_only: throws
    ///   - negotiate/h2_with_h1_fallback: falls back to http_1_1
    ///   - h1_only: uses http_1_1
    future<negotiated_connection> make(abort_source* as) override;
};

/**
 * \brief Adapter: wraps an existing connection_factory as a protocol_connection_factory
 *
 * Use this to wrap legacy connection_factory instances. The protocol is set to the
 * given default (typically http_1_1 for backward compatibility).
 */
class legacy_factory_adapter : public protocol_connection_factory {
    std::unique_ptr<connection_factory> _inner;
    http_protocol _default_protocol;
public:
    legacy_factory_adapter(std::unique_ptr<connection_factory> inner,
                           http_protocol default_proto = http_protocol::http_1_1)
        : _inner(std::move(inner))
        , _default_protocol(default_proto)
    {
    }
    future<negotiated_connection> make(abort_source* as) override {
        auto cs = co_await _inner->make(as);
        co_return negotiated_connection{std::move(cs), _default_protocol};
    }
};

namespace h2 {

/**
 * \brief Dual-protocol HTTP client supporting both HTTP/2 and HTTP/1.1
 *
 * This client transparently handles both HTTP/2 and HTTP/1.1 over the same API.
 * The protocol is determined either by ALPN negotiation (TLS) or explicit
 * configuration (cleartext).
 *
 * **Protocol selection behavior:**
 *
 * | Transport | Preference        | Behavior                                    |
 * |-----------|-------------------|---------------------------------------------|
 * | TLS       | negotiate         | ALPN {"h2","http/1.1"}, use whatever server picks |
 * | TLS       | h2_only           | ALPN {"h2"}, fail if server doesn't support |
 * | TLS       | h1_only           | No ALPN, always HTTP/1.1                    |
 * | TLS       | h2_with_h1_fallback | ALPN {"h2","http/1.1"}, prefer h2         |
 * | Cleartext | h2_only           | h2c with prior knowledge                    |
 * | Cleartext | h1_only           | Plain HTTP/1.1                              |
 * | Cleartext | negotiate         | Not supported over cleartext (defaults h1)  |
 *
 * **Connection management:**
 *
 * Once the protocol is determined (on first connection), all subsequent connections
 * use the same protocol. This avoids mixed-protocol complexity.
 *
 * For HTTP/2: manages 1-N h2_connections with stream multiplexing.
 * For HTTP/1.1: manages a pool of h1 connections (delegated to the existing client).
 *
 * Usage example:
 * \code
 *   auto creds = tls::certificate_credentials::create_default();
 *
 *   // Negotiate h2 vs h1 via ALPN:
 *   h2_client client(addr, creds, "api.example.com");
 *
 *   // Or force h2:
 *   h2_client client(addr, creds, "api.example.com", protocol_preference::h2_only);
 *
 *   // Or force h1:
 *   h2_client client(addr, creds, "api.example.com", protocol_preference::h1_only);
 *
 *   auto req = http::request::make("POST", "api.example.com", "/bid");
 *   req.write_body("json", body_data);
 *
 *   co_await client.make_request(std::move(req),
 *       [] (const http::reply& rep, input_stream<char>&& body) -> future<> {
 *           auto data = co_await util::read_entire_stream_contiguous(body);
 *       },
 *       http::reply::status_type::ok);
 * \endcode
 */
class h2_client {
public:
    /// Callback type for handling responses (same as HTTP/1.1 client)
    using reply_handler = noncopyable_function<future<>(const reply&, input_stream<char>&& body)>;

    /// Whether to retry requests on transport errors
    using retry_requests = bool_class<struct h2_retry_requests_tag>;

    /**
     * \brief Construct a client for plain TCP
     *
     * For cleartext connections, the protocol must be explicitly chosen:
     * - h1_only (default): standard HTTP/1.1
     * - h2_only: h2c with prior knowledge (sends h2 preface immediately)
     * - negotiate: not meaningful over cleartext, behaves as h1_only
     *
     * \param addr — server address
     * \param pref — protocol preference (default: h1_only for cleartext)
     * \param max_connections — max connections (default: 100 for h1, 1 for h2)
     */
    explicit h2_client(socket_address addr,
                       protocol_preference pref = protocol_preference::h1_only,
                       unsigned max_connections = 0);

    /**
     * \brief Construct a secure client with TLS and protocol negotiation
     *
     * Connects via TLS and uses ALPN to negotiate HTTP protocol version.
     *
     * \param addr — server address
     * \param creds — TLS credentials
     * \param host — SNI hostname
     * \param pref — protocol preference (default: negotiate via ALPN)
     * \param max_connections — 0 = auto (1 for h2, 100 for h1)
     */
    h2_client(socket_address addr,
              shared_ptr<tls::certificate_credentials> creds,
              sstring host,
              protocol_preference pref = protocol_preference::negotiate,
              unsigned max_connections = 0);

    /**
     * \brief Construct with a protocol-aware connection factory
     *
     * The factory reports which protocol was negotiated for each connection.
     *
     * \param f — protocol-aware factory
     * \param max_connections — 0 = auto (1 for h2, 100 for h1)
     * \param retry — whether to retry on transport errors
     */
    explicit h2_client(std::unique_ptr<protocol_connection_factory> f,
                       unsigned max_connections = 0,
                       retry_requests retry = retry_requests::no);

    /**
     * \brief Construct with a legacy connection_factory (backward compatible)
     *
     * Wraps the existing connection_factory. The protocol is determined by default_proto.
     *
     * \param f — legacy factory
     * \param default_proto — protocol to use (default: auto based on max_connections)
     * \param max_connections — max connections
     * \param retry — whether to retry on transport errors
     */
    explicit h2_client(std::unique_ptr<connection_factory> f,
                       http_protocol default_proto = http_protocol::http_1_1,
                       unsigned max_connections = 0,
                       retry_requests retry = retry_requests::no);

    ~h2_client();

    h2_client(const h2_client&) = delete;
    h2_client& operator=(const h2_client&) = delete;
    h2_client(h2_client&&) noexcept;
    h2_client& operator=(h2_client&&) noexcept;

    /**
     * \brief Send a request and handle the response
     *
     * Works identically regardless of the underlying protocol (h1 or h2).
     *
     * For HTTP/2: submits the request as an h2 stream. If all connections are
     * at max_concurrent_streams, waits for a slot or opens a new connection.
     *
     * For HTTP/1.1: borrows a connection from the pool (or opens a new one)
     * and sends a standard HTTP/1.1 request.
     *
     * \param req — request to send (moved)
     * \param handle — callback invoked with response headers and body stream
     * \param expected — if set, non-matching status code causes an exception
     * \param as — optional abort source
     */
    future<> make_request(request&& req,
                          reply_handler&& handle,
                          std::optional<reply::status_type>&& expected = std::nullopt,
                          abort_source* as = nullptr);

    /**
     * \brief Send a request and handle the response (non-owning variant)
     */
    future<> make_request(request& req,
                          reply_handler& handle,
                          std::optional<reply::status_type> expected = std::nullopt,
                          abort_source* as = nullptr);

    /**
     * \brief Close all connections gracefully
     *
     * For h2: sends GOAWAY and drains pending streams.
     * For h1: closes pooled connections.
     * Must be called before destruction.
     */
    future<> close();

    /**
     * \brief Update the maximum number of connections
     */
    future<> set_maximum_connections(unsigned nr);

    /// The currently active protocol (unknown until first connection)
    http_protocol active_protocol() const noexcept { return _active_protocol; }

    /// Total number of connections (h1 pool + h2)
    unsigned connections_nr() const noexcept;

    /// Total number of connection factory invocations
    unsigned long total_new_connections_nr() const noexcept;

    /// Total number of active h2 streams (0 if using h1)
    size_t total_active_streams() const noexcept;

private:
    static constexpr unsigned default_h1_max_connections = 100;
    static constexpr unsigned default_h2_max_connections = 1;

    /// Resolve max_connections=0 to the appropriate default for the protocol
    unsigned resolve_max_connections(unsigned requested, http_protocol proto) const noexcept;

    // --- Protocol detection and routing ---

    /// On first connection, determine which protocol to use
    future<> ensure_protocol_determined(abort_source* as);

    /// Route a request to the appropriate protocol handler
    future<> do_make_request_h1(request& req, reply_handler& handle,
                                std::optional<reply::status_type> expected,
                                abort_source* as);
    future<> do_make_request_h2(request& req, reply_handler& handle,
                                std::optional<reply::status_type> expected,
                                abort_source* as);

    // --- h2 connection management ---
    future<shared_ptr<h2_connection>> get_h2_connection(abort_source* as);
    future<shared_ptr<h2_connection>> make_new_h2_connection(abort_source* as);
    future<> do_h2_request(h2_connection& con, request& req, reply_handler& handle,
                           abort_source* as, std::optional<reply::status_type> expected);

    // --- State ---

    std::unique_ptr<protocol_connection_factory> _factory;
    protocol_preference _preference;
    http_protocol _active_protocol = http_protocol::unknown;
    unsigned _max_connections;
    unsigned long _total_new_connections = 0;
    const retry_requests _retry;

    // h1 backend (lazily initialized if protocol resolves to h1)
    // Uses the existing seastar::http::experimental::client under the hood
    std::unique_ptr<client> _h1_client;

    // h2 backend (lazily initialized if protocol resolves to h2)
    std::vector<shared_ptr<h2_connection>> _h2_connections;
    unsigned _h2_nr_connections = 0;
    condition_variable _h2_wait_stream;
};

} // namespace h2

} // namespace seastar::http::experimental
