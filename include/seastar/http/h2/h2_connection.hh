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

#include <seastar/core/condition-variable.hh>
#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/http/h2/h2_stream.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>
#include <seastar/net/api.hh>

#include <nghttp2/nghttp2.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace seastar::http::experimental::h2 {

class h2_client;

/**
 * \brief Connection reference tracker for h2_client
 *
 * Increments the owning h2_client's connection count on construction,
 * decrements on destruction. Used by h2_connection to track its lifetime.
 */
class h2_client_ref {
    h2_client* _c;
public:
    explicit h2_client_ref(h2_client* c) noexcept;
    ~h2_client_ref();
    h2_client_ref(h2_client_ref&& o) noexcept;
    h2_client_ref(const h2_client_ref&) = delete;
    h2_client_ref& operator=(const h2_client_ref&) = delete;
};

/**
 * \brief HTTP/2 multiplexing connection over a single transport socket
 *
 * Manages an nghttp2_session and multiplexes many concurrent h2_streams over
 * a single connected_socket. Two internal fibers (read_loop, send_loop) drive
 * I/O between the socket and nghttp2's state machine.
 *
 * Typical lifecycle:
 *   1. Construct with a connected_socket (already TLS-handshaken with ALPN "h2")
 *   2. Call initialize() to send the client connection preface and SETTINGS
 *   3. Call submit_request() to open streams
 *   4. Call close() to send GOAWAY and drain
 *
 * Users normally interact via h2_client, not this class directly.
 */
class h2_connection : public enable_shared_from_this<h2_connection> {
    friend class h2_client;

public:
    h2_connection(connected_socket&& fd, h2_client_ref cr);
    ~h2_connection();

    h2_connection(const h2_connection&) = delete;
    h2_connection& operator=(const h2_connection&) = delete;

    /**
     * \brief Initialize the HTTP/2 connection
     *
     * Sends the client connection preface (the 24-byte magic and initial SETTINGS)
     * and starts the read and send loops as background fibers.
     *
     * Must be called exactly once after construction and before submit_request().
     */
    future<> initialize();

    /**
     * \brief Submit a request on a new stream
     *
     * Creates a new h2_stream, submits HEADERS (and optionally DATA) via nghttp2,
     * and returns the stream. The caller can then:
     *   - co_await stream->get_reply()         to get response headers
     *   - stream->get_body_stream()             to read the response body
     *
     * \param req — the HTTP request to send (same type as HTTP/1.1 client)
     * \return a shared_ptr to the new stream
     *
     * \throws std::runtime_error if the connection cannot accept more streams
     */
    future<shared_ptr<h2_stream>> submit_request(request& req);

    /// Number of currently active (non-closed) streams
    size_t active_streams() const noexcept { return _streams.size(); }

    /// Whether the connection can accept new streams
    bool can_accept_stream() const noexcept {
        return !_goaway_received
            && !_goaway_sent
            && _streams.size() < _max_concurrent_streams;
    }

    /// Peer's SETTINGS_MAX_CONCURRENT_STREAMS
    uint32_t max_concurrent_streams() const noexcept {
        return _max_concurrent_streams;
    }

    /**
     * \brief Gracefully close the connection
     *
     * Sends GOAWAY, waits for pending streams to finish, then closes the socket.
     */
    future<> close();

    /**
     * \brief Forcefully shut down the connection
     *
     * Resets all active streams and closes the socket immediately.
     */
    void shutdown() noexcept;

private:
    // --- I/O loops (run as background fibers after initialize()) ---

    /// Reads bytes from the socket and feeds them to nghttp2_session_mem_recv()
    future<> read_loop();

    /// Drains nghttp2_session_mem_send() output to the socket
    future<> send_loop();

    /// Signal the send loop that nghttp2 has data to write
    void signal_send();

    /// Send a GOAWAY frame
    future<> send_goaway(uint32_t error_code);

    // --- Internal helpers ---

    /// Build nghttp2 header array from a seastar request
    static std::vector<nghttp2_nv> build_nva(const request& req);

    /// Find stream by id (returns nullptr if not found)
    shared_ptr<h2_stream> find_stream(int32_t stream_id);

    /// Remove stream from tracking
    void remove_stream(int32_t stream_id);

    // --- nghttp2 callbacks (static C functions) ---

    /// Called by nghttp2 when it wants to send data to the network.
    /// We buffer the data and signal the send loop.
    static ssize_t cb_send(nghttp2_session* session,
                           const uint8_t* data, size_t length,
                           int flags, void* user_data);

    /// Called when a complete frame is received
    static int cb_on_frame_recv(nghttp2_session* session,
                                const nghttp2_frame* frame,
                                void* user_data);

    /// Called for each chunk of DATA frame payload
    static int cb_on_data_chunk_recv(nghttp2_session* session,
                                     uint8_t flags,
                                     int32_t stream_id,
                                     const uint8_t* data, size_t len,
                                     void* user_data);

    /// Called when a stream is closed (RST_STREAM or END_STREAM)
    static int cb_on_stream_close(nghttp2_session* session,
                                  int32_t stream_id,
                                  uint32_t error_code,
                                  void* user_data);

    /// Called for each header name-value pair in HEADERS frame
    static int cb_on_header(nghttp2_session* session,
                            const nghttp2_frame* frame,
                            const uint8_t* name, size_t namelen,
                            const uint8_t* value, size_t valuelen,
                            uint8_t flags, void* user_data);

    /// Called at the start of a HEADERS frame
    static int cb_on_begin_headers(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   void* user_data);

    /// Called by nghttp2 to read request body data for DATA frames
    static ssize_t cb_data_source_read(nghttp2_session* session,
                                       int32_t stream_id,
                                       uint8_t* buf, size_t length,
                                       uint32_t* data_flags,
                                       nghttp2_data_source* source,
                                       void* user_data);

    // --- Connection state ---

    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    h2_client_ref _ref;

    /// Called when a stream closes, so the owning h2_client can signal waiters
    std::function<void()> _on_stream_close_notify;

    nghttp2_session* _session = nullptr;
    std::unordered_map<int32_t, shared_ptr<h2_stream>> _streams;

    // SETTINGS received from peer
    uint32_t _max_concurrent_streams = 100;
    uint32_t _initial_window_size = 65535;
    uint32_t _max_frame_size = 16384;
    uint32_t _max_header_list_size = 0;  // 0 = unlimited

    // Send loop coordination
    semaphore _send_pending{0};
    bool _send_loop_running = false;

    // Connection lifecycle
    future<> _read_loop_done = make_ready_future<>();
    future<> _send_loop_done = make_ready_future<>();
    bool _goaway_sent = false;
    bool _goaway_received = false;
    int32_t _last_stream_id = 0;  // for GOAWAY

    // Output buffer for cb_send (accumulated between send loop iterations)
    std::vector<uint8_t> _outbuf;
};

} // namespace seastar::http::experimental::h2
