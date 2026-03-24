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

#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/pipe.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/request.hh>

#include <cstdint>
#include <memory>

namespace seastar::http::experimental::h2 {

/**
 * \brief Represents a single HTTP/2 stream within an h2 connection
 *
 * Each stream carries one request-response pair. Streams are multiplexed over
 * a single TCP connection. The stream manages its own flow control window and
 * delivers the response body via a pipe-backed input_stream.
 *
 * Users do not create streams directly; they are created by h2_connection::submit_request().
 */
class h2_stream : public enable_shared_from_this<h2_stream> {
    friend class h2_connection;

public:
    /// HTTP/2 stream states (RFC 9113 Section 5.1)
    enum class state : uint8_t {
        idle,
        open,
        half_closed_local,
        half_closed_remote,
        closed,
        reset
    };

    using reply_ptr = std::unique_ptr<reply>;

    h2_stream(int32_t stream_id);
    ~h2_stream();

    /// Stream identifier (odd for client-initiated)
    int32_t id() const noexcept { return _stream_id; }

    /// Current stream state
    state current_state() const noexcept { return _state; }

    /**
     * \brief Wait for the response headers
     *
     * Returns a future that resolves when the server sends back the HEADERS frame
     * for this stream. The reply contains status code and headers but not the body.
     */
    future<reply_ptr> get_reply();

    /**
     * \brief Get an input_stream to read the response body
     *
     * The returned stream yields DATA frame payloads in order. Reading to EOF
     * indicates the server sent END_STREAM.
     *
     * Must be called after get_reply() resolves.
     */
    input_stream<char> get_body_stream();

    /**
     * \brief Cancel this stream
     *
     * Sends RST_STREAM to the peer and transitions to the reset state.
     * Any pending futures will resolve with an exception.
     */
    void reset(uint32_t error_code);

private:
    // --- Called by h2_connection's nghttp2 callbacks ---

    /// Called when a response header is received (:status, regular headers)
    void on_header(sstring name, sstring value);

    /// Called when the HEADERS frame is complete (END_HEADERS flag)
    void on_headers_complete();

    /// Called for each DATA frame chunk
    void on_data(temporary_buffer<char> data);

    /// Called when END_STREAM is received (on HEADERS or DATA)
    void on_end_stream();

    /// Called on RST_STREAM or connection error
    void on_error(uint32_t error_code);

    int32_t _stream_id;
    state _state = state::idle;

    // Response headers
    reply_ptr _reply;
    promise<reply_ptr> _reply_promise;
    bool _headers_received = false;

    // Response body delivery
    std::optional<pipe<temporary_buffer<char>>> _body_pipe;

    // Flow control
    int32_t _local_window = 65535;    // our receive window
    int32_t _remote_window = 65535;   // peer's receive window (for sending)
};

} // namespace seastar::http::experimental::h2
