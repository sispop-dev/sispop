// Copyright (c) 2018-2020, The Loki Project
// Copyright (c) 2014-2019, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#pragma once

#include "net/http_server_impl_base.h"
#include "common/command_line.h"
#include "core_rpc_server.h"

namespace cryptonote { namespace rpc {

  using http_response_code = std::pair<int, std::string_view>;

  /************************************************************************/
  /* Core HTTP RPC server                                                 */
  /************************************************************************/
  class http_server: public epee::http_server_impl_base<http_server>
  {
  public:
    static constexpr int DEFAULT_RPC_THREADS = 2;
    static const command_line::arg_descriptor<std::string, false, true, 2> arg_rpc_bind_port;
    static const command_line::arg_descriptor<std::string> arg_rpc_restricted_bind_port;
    static const command_line::arg_descriptor<bool> arg_restricted_rpc;
    static const command_line::arg_descriptor<std::string> arg_rpc_ssl;
    static const command_line::arg_descriptor<std::string> arg_rpc_ssl_private_key;
    static const command_line::arg_descriptor<std::string> arg_rpc_ssl_certificate;
    static const command_line::arg_descriptor<std::string> arg_rpc_ssl_ca_certificates;
    static const command_line::arg_descriptor<std::vector<std::string>> arg_rpc_ssl_allowed_fingerprints;
    static const command_line::arg_descriptor<bool> arg_rpc_ssl_allow_any_cert;
    static const command_line::arg_descriptor<bool> arg_public_node;
    static const command_line::arg_descriptor<int> arg_rpc_long_poll_connections;

    typedef epee::net_utils::connection_context_base connection_context;

    http_server(core_rpc_server& server) : m_server{server} {}

    static void init_options(boost::program_options::options_description& desc);

    bool init(
        const boost::program_options::variables_map& vm,
        const bool restricted,
        const std::string& port
      );

    bool handle_http_request(
        const epee::net_utils::http::http_request_info& query_info,
        epee::net_utils::http::http_response_info& response,
        connection_context& context) override;

    http_response_code handle_http(
      const epee::net_utils::http::http_request_info& query_info,
      epee::net_utils::http::http_response_info& response_info,
      connection_context& context);

    http_response_code handle_json_rpc_request(
      const epee::net_utils::http::http_request_info& query_info,
      epee::net_utils::http::http_response_info& response_info,
      connection_context& context,
      rpc_request& request);

    int m_max_long_poll_connections;
  private:
    core_rpc_server& m_server;
    bool m_restricted;
    std::atomic<int> m_long_poll_active_connections;
  };

}} // namespace cryptonote::rpc
