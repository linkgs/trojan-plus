/*
 * This file is part of the trojan project.
 * Trojan is an unidentifiable mechanism that helps you bypass GFW.
 * Copyright (C) 2017-2020  The Trojan Authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TROJAN_UTILS_H_
#define _TROJAN_UTILS_H_

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <list>
#include <string>

#include "log.h"

#ifdef ENABLE_REUSE_PORT
typedef boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT> reuse_port;
#endif  // ENABLE_REUSE_PORT

#ifndef IP_RECVTTL
#define IP_RECVTTL 12
#endif

#ifndef IPV6_RECVHOPLIMIT
#define IPV6_RECVHOPLIMIT 51
#endif

#ifndef IPV6_HOPLIMIT
#define IPV6_HOPLIMIT 21
#endif

#ifndef IP_TTL
#define IP_TTL 4
#endif

// copied from shadowsocks-libe udprelay.h
#ifndef IP_TRANSPARENT
#define IP_TRANSPARENT 19
#endif

#ifndef IP_RECVORIGDSTADDR
#ifdef IP_ORIGDSTADDR
#define IP_RECVORIGDSTADDR IP_ORIGDSTADDR
#else
#define IP_RECVORIGDSTADDR 20
#endif
#endif

#ifndef IPV6_RECVORIGDSTADDR
#ifdef IPV6_ORIGDSTADDR
#define IPV6_RECVORIGDSTADDR IPV6_ORIGDSTADDR
#else
#define IPV6_RECVORIGDSTADDR 74
#endif
#endif

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

#ifndef SOL_IPV6
#define SOL_IPV6 IPPROTO_IPV6
#endif

#define PACKET_HEADER_SIZE (1 + 28 + 2 + 64)
#define DEFAULT_PACKET_SIZE 1397  // 1492 - PACKET_HEADER_SIZE = 1397, the default MTU for UDP relay

template <typename ThisT, typename EndPoint>
void connect_out_socket(ThisT this_ptr, std::string addr, std::string port, boost::asio::ip::tcp::resolver& resolver,
                        boost::asio::ip::tcp::socket& out_socket, EndPoint in_endpoint, std::function<void()> connected_handler) {
    resolver.async_resolve(addr, port, [=, &out_socket](const boost::system::error_code error, boost::asio::ip::tcp::resolver::results_type results) {
        if (error || results.empty()) {
            _log_with_endpoint(in_endpoint, "cannot resolve remote server hostname " + addr + ":" + port + " reason: " + error.message(), Log::ERROR);
            this_ptr->destroy();
            return;
        }
        auto iterator = results.begin();
        _log_with_endpoint(in_endpoint, addr + " is resolved to " + iterator->endpoint().address().to_string(), Log::ALL);
        boost::system::error_code ec;
        out_socket.open(iterator->endpoint().protocol(), ec);
        if (ec) {
            this_ptr->destroy();
            return;
        }
        if (this_ptr->config.tcp.no_delay) {
            out_socket.set_option(boost::asio::ip::tcp::no_delay(true));
        }
        if (this_ptr->config.tcp.keep_alive) {
            out_socket.set_option(boost::asio::socket_base::keep_alive(true));
        }
#ifdef TCP_FASTOPEN_CONNECT
        if (this_ptr->config.tcp.fast_open) {
            using fastopen_connect = boost::asio::detail::socket_option::boolean<IPPROTO_TCP, TCP_FASTOPEN_CONNECT>;
            boost::system::error_code ec;
            out_socket.set_option(fastopen_connect(true), ec);
        }
#endif  // TCP_FASTOPEN_CONNECT
        auto timeout_timer = std::shared_ptr<boost::asio::steady_timer>(nullptr);
        if (this_ptr->config.tcp.connect_time_out > 0) {
            // out_socket.async_connect will be stuck forever when the host is not reachable
            // we must set a timeout timer
            timeout_timer = std::make_shared<boost::asio::steady_timer>(this_ptr->io_context);
            timeout_timer->expires_after(std::chrono::seconds(this_ptr->config.tcp.connect_time_out));
            timeout_timer->async_wait([=](const boost::system::error_code error) {
                if (!error) {
                    _log_with_endpoint(in_endpoint, "cannot establish connection to remote server " + addr + ':' + port + " reason: timeout", Log::ERROR);
                    this_ptr->destroy();
                }
            });
        }

        out_socket.async_connect(*iterator, [=](const boost::system::error_code error) {
            if (timeout_timer) {
                timeout_timer->cancel();
            }

            if (error) {
                _log_with_endpoint(in_endpoint, "cannot establish connection to remote server " + addr + ':' + port + " reason: " + error.message(), Log::ERROR);
                this_ptr->destroy();
                return;
            }

            connected_handler();
        });
    });
}

template <typename ThisT, typename EndPoint>
void connect_remote_server_ssl(ThisT this_ptr, std::string addr, std::string port, boost::asio::ip::tcp::resolver& resolver,
                               boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& out_socket, EndPoint in_endpoint, std::function<void()> connected_handler) {
    connect_out_socket(this_ptr, addr, port, resolver, out_socket.next_layer(), in_endpoint, [=, &out_socket]() {
        out_socket.async_handshake(boost::asio::ssl::stream_base::client, [=, &out_socket](const boost::system::error_code error) {
            if (error) {
                _log_with_endpoint(in_endpoint, "SSL handshake failed with " + addr + ':' + port + " reason: " + error.message(), Log::ERROR);
                this_ptr->destroy();
                return;
            }
            _log_with_endpoint(in_endpoint, "tunnel established");
            if (this_ptr->config.ssl.reuse_session) {
                auto ssl = out_socket.native_handle();
                if (!SSL_session_reused(ssl)) {
                    _log_with_endpoint(in_endpoint, "SSL session not reused");
                } else {
                    _log_with_endpoint(in_endpoint, "SSL session reused");
                }
            }
            connected_handler();
        });
    });
}

template <typename ThisPtr>
void shutdown_ssl_socket(ThisPtr this_ptr, boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& socket) {
    if (socket.next_layer().is_open()) {
        auto self = this_ptr->shared_from_this();
        auto ssl_shutdown_timer = std::make_shared<boost::asio::steady_timer>(this_ptr->io_context);
        auto ssl_shutdown_cb = [self, ssl_shutdown_timer, &socket](const boost::system::error_code error) {
            if (error == boost::asio::error::operation_aborted) {
                return;
            }
            boost::system::error_code ec;
            ssl_shutdown_timer.get()->cancel();
            socket.next_layer().cancel(ec);
            socket.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket.next_layer().close(ec);
        };
        boost::system::error_code ec;
        socket.next_layer().cancel(ec);
        socket.async_shutdown(ssl_shutdown_cb);
        ssl_shutdown_timer.get()->expires_after(std::chrono::seconds(30));
        ssl_shutdown_timer.get()->async_wait(ssl_shutdown_cb);
    }
}

std::pair<std::string, uint16_t> recv_tproxy_udp_msg(int fd, boost::asio::ip::udp::endpoint& recv_endpoint, char* buf, int& buf_len, int& ttl);
bool prepare_nat_udp_bind(int fd, bool is_ipv4, bool recv_ttl);
bool prepare_nat_udp_target_bind(int fd, bool is_ipv4, const boost::asio::ip::udp::endpoint& udp_target_endpoint);

#endif  //_TROJAN_UTILS_H_