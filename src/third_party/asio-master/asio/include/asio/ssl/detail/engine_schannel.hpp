/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#if MONGO_CONFIG_SSL_PROVIDER != SSL_PROVIDER_WINDOWS
#error Only include this file in the SChannel Implementation
#endif

namespace asio {
namespace ssl {
namespace detail {

class engine
{
public:
  enum want
  {
    // Returned by functions to indicate that the engine wants input. The input
    // buffer should be updated to point to the data. The engine then needs to
    // be called again to retry the operation.
    want_input_and_retry = -2,

    // Returned by functions to indicate that the engine wants to write output.
    // The output buffer points to the data to be written. The engine then
    // needs to be called again to retry the operation.
    want_output_and_retry = -1,

    // Returned by functions to indicate that the engine doesn't need input or
    // output.
    want_nothing = 0,

    // Returned by functions to indicate that the engine wants to write output.
    // The output buffer points to the data to be written. After that the
    // operation is complete, and the engine does not need to be called again.
    want_output = 1
  };

  // Construct a new engine for the specified context.
  ASIO_DECL explicit engine(SCHANNEL_CRED* context);

  // Destructor.
  ASIO_DECL ~engine();

  // Get the underlying implementation in the native type.
  ASIO_DECL PCtxtHandle native_handle();

  // Set the peer verification mode.
  ASIO_DECL asio::error_code set_verify_mode(
      verify_mode v, asio::error_code& ec);

  // Set the peer verification depth.
  ASIO_DECL asio::error_code set_verify_depth(
      int depth, asio::error_code& ec);

  // Set a peer certificate verification callback.
  ASIO_DECL asio::error_code set_verify_callback(
      verify_callback_base* callback, asio::error_code& ec);

  // Perform an SSL handshake using either SSL_connect (client-side) or
  // SSL_accept (server-side).
  ASIO_DECL want handshake(
      stream_base::handshake_type type, asio::error_code& ec);

  // Perform a graceful shutdown of the SSL session.
  ASIO_DECL want shutdown(asio::error_code& ec);

  // Write bytes to the SSL session.
  ASIO_DECL want write(const asio::const_buffer& data,
      asio::error_code& ec, std::size_t& bytes_transferred);

  // Read bytes from the SSL session.
  ASIO_DECL want read(const asio::mutable_buffer& data,
      asio::error_code& ec, std::size_t& bytes_transferred);

  // Get output data to be written to the transport.
  ASIO_DECL asio::mutable_buffer get_output(
      const asio::mutable_buffer& data);

  // Put input data that was read from the transport.
  ASIO_DECL asio::const_buffer put_input(
      const asio::const_buffer& data);

  // Map an error::eof code returned by the underlying transport according to
  // the type and state of the SSL session. Returns a const reference to the
  // error code object, suitable for passing to a completion handler.
  ASIO_DECL const asio::error_code& map_error_code(
      asio::error_code& ec) const;

private:
  // Disallow copying and assignment.
  engine(const engine&);
  engine& operator=(const engine&);

private:

};

} // namespace detail
} // namespace ssl
} // namespace asio
