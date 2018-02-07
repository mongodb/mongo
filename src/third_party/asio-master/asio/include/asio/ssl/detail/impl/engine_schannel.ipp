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

engine::engine(SCHANNEL_CRED* context)
{
}

engine::~engine()
{
}

PCtxtHandle engine::native_handle()
{
  return nullptr;
}

asio::error_code engine::set_verify_mode(
    verify_mode v, asio::error_code& ec)
{
  ec = asio::error_code();
  return ec;
}

asio::error_code engine::set_verify_depth(
    int depth, asio::error_code& ec)
{
  ec = asio::error_code();
  return ec;
}

asio::error_code engine::set_verify_callback(
    verify_callback_base* callback, asio::error_code& ec)
{
  ec = asio::error_code();
  return ec;
}


engine::want engine::handshake(
    stream_base::handshake_type type, asio::error_code& ec)
{
  return want::want_nothing;
}

engine::want engine::shutdown(asio::error_code& ec)
{
  return want::want_nothing;
}

engine::want engine::write(const asio::const_buffer& data,
    asio::error_code& ec, std::size_t& bytes_transferred)
{
  return want::want_nothing;
}

engine::want engine::read(const asio::mutable_buffer& data,
    asio::error_code& ec, std::size_t& bytes_transferred)
{
  return want::want_nothing;
}

asio::mutable_buffer engine::get_output(
    const asio::mutable_buffer& data)
{
    return asio::mutable_buffer(nullptr, 0);
}

asio::const_buffer engine::put_input(
    const asio::const_buffer& data)
{
    return asio::const_buffer(nullptr, 0);
}

const asio::error_code& engine::map_error_code(
    asio::error_code& ec) const
{
  return ec;
}

} // namespace detail
} // namespace ssl
} // namespace asio
