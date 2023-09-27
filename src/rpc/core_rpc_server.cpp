// Copyright (c) 2014-2019, The Monero Project
// Copyright (c)      2018-2023, The Oxen Project
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

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/endian/conversion.hpp>
#include <algorithm>
#include <cstring>
#include <variant>
#include "include_base_utils.h"
#include "string_tools.h"
#include "core_rpc_server.h"
#include "common/command_line.h"
#include "common/updates.h"
#include "common/download.h"
#include "common/sispop.h"
#include "common/sha256sum.h"
#include "common/perf_timer.h"
#include "common/random.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_basic/account.h"
#include "cryptonote_basic/cryptonote_basic_impl.h"
#include "cryptonote_core/tx_sanity_check.h"
#include "misc_language.h"
#include "net/parse.h"
#include "storages/http_abstract_invoke.h"
#include "crypto/hash.h"
#include "rpc/rpc_args.h"
#include "rpc/rpc_handler.h"
#include "core_rpc_server_error_codes.h"
#include "p2p/net_node.h"
#include "version.h"

#undef SISPOP_DEFAULT_LOG_CATEGORY
#define SISPOP_DEFAULT_LOG_CATEGORY "daemon.rpc"


namespace cryptonote { namespace rpc {

  namespace {
    // Helper loaders for RPC registration; this lets us reduce the amount of compiled code by
    // avoiding the need to instantiate {JSON,binary} loading code for {binary,JSON} commands.
    // This first one is for JSON, the specialization below is for binary.
    template <typename RPC, typename JSON = void>
    struct reg_helper {
      using Request = typename RPC::request;

      Request load(rpc_request& request) {
        Request req{};
        if (std::holds_alternative<std::string_view>(request.body)) {
          if (!epee::serialization::load_t_from_json(req, std::get<std::string_view>(request.body)))
            throw parse_error{"Failed to parse JSON parameters"};
        } else {
          // This is nasty.  TODO: get rid of epee's horrible serialization code.
          auto& epee_stuff = std::get<jsonrpc_params>(request.body);
          auto& storage_entry = epee_stuff.second;
          // For some reason epee calls a json object a "section" instead of something common like
          // dict, object, hash, map.  But okay, then it calls a pointer to a section a "hsection"
          // because obfuscation is the epee way.  Then we have `array_entry` (and of course,
          // `harray` to refer to an `array_entry*`), but array_entries are *only* allowed to be
          // arrays of sections.  Meanwhile epee's author left comments telling us that XML is
          // horrible.  Pot meet kettle.
          if (storage_entry.type() == typeid(epee::serialization::section)) {
            auto* section = &boost::get<epee::serialization::section>(storage_entry);
            req.load(epee_stuff.first, section);
          }
          else if (storage_entry.type() == typeid(epee::serialization::array_entry)) {
            throw std::runtime_error("FIXME 125157015");
          }
        }
        return req;
      }

      // store_t_to_json can't store a string.  Go epee.
      template <typename R = typename RPC::response, std::enable_if_t<std::is_same<R, std::string>::value, int> = 0>
      std::string serialize(std::string&& res) {
        std::ostringstream o;
        epee::serialization::dump_as_json(o, std::move(res), 0 /*indent*/, false /*newlines*/);
        return o.str();
      }

      template <typename R = typename RPC::response, std::enable_if_t<!std::is_same<R, std::string>::value, int> = 0>
      std::string serialize(typename RPC::response&& res) {
        std::string response;
        epee::serialization::store_t_to_json(res, response, 0 /*indent*/, false /*newlines*/);
        return response;
      }
    };

    // binary command specialization
    template <typename RPC>
    struct reg_helper<RPC, std::enable_if_t<std::is_base_of<BINARY, RPC>::value>> {
      using Request = typename RPC::request;
      Request load(rpc_request& request) { 
        Request req{};
        if (!std::holds_alternative<std::string_view>(request.body))
          throw std::runtime_error{"Internal error: can't load binary a RPC command with non-string body"};
        auto data = std::get<std::string_view>(request.body);
        if (!epee::serialization::load_t_from_binary(req, data))
          throw parse_error{"Failed to parse binary data parameters"};
        return req;
      }

      std::string serialize(typename RPC::response&& res) {
        std::string response;
        epee::serialization::store_t_to_binary(res, response);
        return response;
      }
    };

    template <typename RPC>
    void register_rpc_command(std::unordered_map<std::string, std::shared_ptr<const rpc_command>>& regs)
    {
      using Request = typename RPC::request;
      using Response = typename RPC::response;
      /// check that core_rpc_server.invoke(Request, rpc_context) returns a Response; the code below
      /// will fail anyway if this isn't satisfied, but that compilation failure might be more cryptic.
      using invoke_return_type = decltype(std::declval<core_rpc_server>().invoke(std::declval<Request&&>(), rpc_context{}));
      static_assert(std::is_same<Response, invoke_return_type>::value,
          "Unable to register RPC command: core_rpc_server::invoke(Request) is not defined or does not return a Response");
      auto cmd = std::make_shared<rpc_command>();
      constexpr bool binary = std::is_base_of<BINARY, RPC>::value;
      cmd->is_public = std::is_base_of<PUBLIC, RPC>::value;
      cmd->is_binary = binary;
      cmd->invoke = [](rpc_request&& request, core_rpc_server& server) {
        reg_helper<RPC> helper;
        Response res = server.invoke(helper.load(request), std::move(request.context));
        return helper.serialize(std::move(res));
      };

      for (const auto& name : RPC::names())
        regs.emplace(name, cmd);
    }

    template <typename... RPC>
    std::unordered_map<std::string, std::shared_ptr<const rpc_command>> register_rpc_commands(rpc::type_list<RPC...>) {
      std::unordered_map<std::string, std::shared_ptr<const rpc_command>> regs;

      (register_rpc_command<RPC>(regs), ...);


namespace cryptonote { namespace rpc {

  namespace {
    // Helper loaders for RPC registration; this lets us reduce the amount of compiled code by
    // avoiding the need to instantiate {JSON,binary} loading code for {binary,JSON} commands.
    // This first one is for JSON, the specialization below is for binary.
    template <typename RPC, typename JSON = void>
    struct reg_helper {
      using Request = typename RPC::request;

      Request load(rpc_request& request) {
        Request req{};
        if (std::holds_alternative<std::string_view>(request.body)) {
          if (!epee::serialization::load_t_from_json(req, std::get<std::string_view>(request.body)))
            throw parse_error{"Failed to parse JSON parameters"};
        } else {
          // This is nasty.  TODO: get rid of epee's horrible serialization code.
          auto& epee_stuff = std::get<jsonrpc_params>(request.body);
          auto& storage_entry = epee_stuff.second;
          // Epee nomenclature translactions:
          //
          // - "storage_entry" is a variant over values (ints, doubles, string, storage_entries, or
          // array_entry).
          //
          // - "array_entry" is a variant over vectors of all of those values.
          //
          // Epee's json serialization also has a metric ton of limitations: for example it can't
          // properly deserialize signed integer (unless *all* values are negative), or doubles
          // (unless *all* values do not look like ints), and for both serialization and
          // deserialization doesn't support lists of lists, and any mixed types in lists (for
          // example '[bool, 1, "hi"]`).
          //
          // Conclusion: it needs to go.
          if (auto* section = std::get_if<epee::serialization::section>(&storage_entry))
            req.load(epee_stuff.first, section);
          else
            throw std::runtime_error{"only top-level JSON object values are currently supported"};
        }
        return req;
      }

      // store_t_to_json can't store a string.  Go epee.
      template <typename R = typename RPC::response, std::enable_if_t<std::is_same<R, std::string>::value, int> = 0>
      std::string serialize(std::string&& res) {
        std::ostringstream o;
        epee::serialization::dump_as_json(o, std::move(res), 0 /*indent*/, false /*newlines*/);
        return o.str();
      }

      template <typename R = typename RPC::response, std::enable_if_t<!std::is_same<R, std::string>::value, int> = 0>
      std::string serialize(typename RPC::response&& res) {
        std::string response;
        epee::serialization::store_t_to_json(res, response, 0 /*indent*/, false /*newlines*/);
        return response;
      }
    };

    // binary command specialization
    template <typename RPC>
    struct reg_helper<RPC, std::enable_if_t<std::is_base_of<BINARY, RPC>::value>> {
      using Request = typename RPC::request;
      Request load(rpc_request& request) { 
        Request req{};
        if (!std::holds_alternative<std::string_view>(request.body))
          throw std::runtime_error{"Internal error: can't load binary a RPC command with non-string body"};
        auto data = std::get<std::string_view>(request.body);
        if (!epee::serialization::load_t_from_binary(req, data))
          throw parse_error{"Failed to parse binary data parameters"};
        return req;
      }

      std::string serialize(typename RPC::response&& res) {
        std::string response;
        epee::serialization::store_t_to_binary(res, response);
        return response;
      }
    };

    template <typename RPC>
    void register_rpc_command(std::unordered_map<std::string, std::shared_ptr<const rpc_command>>& regs)
    {
      using Request = typename RPC::request;
      using Response = typename RPC::response;
      /// check that core_rpc_server.invoke(Request, rpc_context) returns a Response; the code below
      /// will fail anyway if this isn't satisfied, but that compilation failure might be more cryptic.
      using invoke_return_type = decltype(std::declval<core_rpc_server>().invoke(std::declval<Request&&>(), rpc_context{}));
      static_assert(std::is_same<Response, invoke_return_type>::value,
          "Unable to register RPC command: core_rpc_server::invoke(Request) is not defined or does not return a Response");
      auto cmd = std::make_shared<rpc_command>();
      constexpr bool binary = std::is_base_of<BINARY, RPC>::value;
      cmd->is_public = std::is_base_of<PUBLIC, RPC>::value;
      cmd->is_binary = binary;
      cmd->invoke = [](rpc_request&& request, core_rpc_server& server) {
        reg_helper<RPC> helper;
        Response res = server.invoke(helper.load(request), std::move(request.context));
        return helper.serialize(std::move(res));
      };

      for (const auto& name : RPC::names())
        regs.emplace(name, cmd);
    }

    template <typename... RPC>
    std::unordered_map<std::string, std::shared_ptr<const rpc_command>> register_rpc_commands(rpc::type_list<RPC...>) {
      std::unordered_map<std::string, std::shared_ptr<const rpc_command>> regs;

      (register_rpc_command<RPC>(regs), ...);

      return regs;
    }

    constexpr size_t MAX_RESTRICTED_GLOBAL_FAKE_OUTS_COUNT = 5000;
    constexpr uint64_t OUTPUT_HISTOGRAM_RECENT_CUTOFF_RESTRICTION = 3 * 86400; // 3 days max, the wallet requests 1.8 days
    constexpr uint64_t round_up(uint64_t value, uint64_t quantum) { return (value + quantum - 1) / quantum * quantum; }

  }

  const std::unordered_map<std::string, std::shared_ptr<const rpc_command>> rpc_commands = register_rpc_commands(rpc::core_rpc_types{});

  namespace string_tools = epee::string_tools;

  const command_line::arg_descriptor<std::string> core_rpc_server::arg_bootstrap_daemon_address = {
      "bootstrap-daemon-address"
    , "URL of a 'bootstrap' remote daemon that the connected wallets can use while this daemon is still not fully synced.\n"
      "Use 'auto' to enable automatic public nodes discovering and bootstrap daemon switching"
    , ""
    };

  const command_line::arg_descriptor<std::string> core_rpc_server::arg_bootstrap_daemon_login = {
      "bootstrap-daemon-login"
    , "Specify username:password for the bootstrap daemon login"
    , ""
    };

  //-----------------------------------------------------------------------------------
  void core_rpc_server::init_options(boost::program_options::options_description& desc)
  {
    command_line::add_arg(desc, arg_bootstrap_daemon_address);
    command_line::add_arg(desc, arg_bootstrap_daemon_login);
    cryptonote::rpc_args::init_options(desc, true);
  }
  //------------------------------------------------------------------------------------------------------------------------------
  core_rpc_server::core_rpc_server(
      core& cr
    , nodetool::node_server<cryptonote::t_cryptonote_protocol_handler<cryptonote::core> >& p2p
    )
    : m_core(cr)
    , m_p2p(p2p)
    , m_was_bootstrap_ever_used(false)
  {}
  //------------------------------------------------------------------------------------------------------------------------------
  bool core_rpc_server::set_bootstrap_daemon(const std::string &address, const std::string &username_password)
  {
    std::optional<epee::net_utils::http::login> credentials;
    const auto loc = username_password.find(':');
    if (loc != std::string::npos)
    {
      credentials = epee::net_utils::http::login(username_password.substr(0, loc), username_password.substr(loc + 1));
    }
    return set_bootstrap_daemon(address, credentials);
  }
  //------------------------------------------------------------------------------------------------------------------------------
  std::optional<std::string> core_rpc_server::get_random_public_node()
  {
    GET_PUBLIC_NODES::response response{};
    try
    {
      GET_PUBLIC_NODES::request request{};
      request.gray  = true;
      request.white = true;

      rpc_context context = {};
      context.admin       = true;
      response            = invoke(std::move(request), context);
    }
    catch(const std::exception &e)
    {
      return std::nullopt;
    }

    const auto get_random_node_address = [](const std::vector<public_node>& public_nodes) -> std::string {
      const auto& random_node = public_nodes[crypto::rand_idx(public_nodes.size())];
      const auto address = random_node.host + ":" + std::to_string(random_node.rpc_port);
      return address;
    };

    if (!response.white.empty())
    {
      return get_random_node_address(response.white);
    }

    MDEBUG("No white public node found, checking gray peers");

    if (!response.gray.empty())
    {
      return get_random_node_address(response.gray);
    }

    MERROR("Failed to find any suitable public node");
    return std::nullopt;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool core_rpc_server::set_bootstrap_daemon(const std::string &address, const std::optional<epee::net_utils::http::login> &credentials)
  {
    std::unique_lock lock{m_bootstrap_daemon_mutex};

    if (address.empty())
    {
      m_bootstrap_daemon.reset(nullptr);
    }
    else if (address == "auto")
    {
      m_bootstrap_daemon.reset(new bootstrap_daemon([this]{ return get_random_public_node(); }));
    }
    else
    {
      m_bootstrap_daemon.reset(new bootstrap_daemon(address, credentials));
    }

    m_should_use_bootstrap_daemon = m_bootstrap_daemon.get() != nullptr;

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  void core_rpc_server::init(const boost::program_options::variables_map& vm)
  {
    if (!set_bootstrap_daemon(command_line::get_arg(vm, arg_bootstrap_daemon_address),
                              command_line::get_arg(vm, arg_bootstrap_daemon_login)))
    {
      MERROR("Failed to parse bootstrap daemon address");
    }
    m_was_bootstrap_ever_used = false;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool core_rpc_server::check_core_ready()
  {
    return m_p2p.get_payload_object().is_synchronized();
  }


#define CHECK_CORE_READY() do { if(!check_core_ready()){ res.status =  STATUS_BUSY; return res; } } while(0)

  //------------------------------------------------------------------------------------------------------------------------------
  GET_HEIGHT::response core_rpc_server::invoke(GET_HEIGHT::request&& req, rpc_context context)
  {
    GET_HEIGHT::response res{};

    PERF_TIMER(on_get_height);
    if (use_bootstrap_daemon_if_necessary<GET_HEIGHT>(req, res))
      return res;

    crypto::hash hash;
    m_core.get_blockchain_top(res.height, hash);
    ++res.height; // block height to chain height
    res.hash = string_tools::pod_to_hex(hash);
    res.status = STATUS_OK;

    res.immutable_height = 0;
    cryptonote::checkpoint_t checkpoint;
    if (m_core.get_blockchain_storage().get_db().get_immutable_checkpoint(&checkpoint, res.height - 1))
    {
      res.immutable_height = checkpoint.height;
      res.immutable_hash   = string_tools::pod_to_hex(checkpoint.block_hash);
    }

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_INFO::response core_rpc_server::invoke(GET_INFO::request&& req, rpc_context context)
  {
    GET_INFO::response res{};

    PERF_TIMER(on_get_info);
    if (use_bootstrap_daemon_if_necessary<GET_INFO>(req, res))
    {
      {
        std::shared_lock lock{m_bootstrap_daemon_mutex};
        if (m_bootstrap_daemon.get() != nullptr)
        {
          res.bootstrap_daemon_address = m_bootstrap_daemon->address();
        }
      }
      crypto::hash top_hash;
      m_core.get_blockchain_top(res.height_without_bootstrap, top_hash);
      ++res.height_without_bootstrap; // turn top block height into blockchain height
      res.was_bootstrap_ever_used = true;
      return res;
    }

    const bool restricted = !context.admin;

    crypto::hash top_hash;
    m_core.get_blockchain_top(res.height, top_hash);
    ++res.height; // turn top block height into blockchain height
    res.top_block_hash = string_tools::pod_to_hex(top_hash);
    res.target_height = m_core.get_target_blockchain_height();

    res.immutable_height = 0;
    cryptonote::checkpoint_t checkpoint;
    if (m_core.get_blockchain_storage().get_db().get_immutable_checkpoint(&checkpoint, res.height - 1))
    {
      res.immutable_height     = checkpoint.height;
      res.immutable_block_hash = string_tools::pod_to_hex(checkpoint.block_hash);
    }

    res.difficulty = m_core.get_blockchain_storage().get_difficulty_for_next_block();
    res.target = m_core.get_blockchain_storage().get_difficulty_target();
    res.tx_count = m_core.get_blockchain_storage().get_total_transactions() - res.height; //without coinbase
    res.tx_pool_size = m_core.get_pool().get_transactions_count();
    res.alt_blocks_count = restricted ? 0 : m_core.get_blockchain_storage().get_alternative_blocks_count();
    uint64_t total_conn = restricted ? 0 : m_p2p.get_public_connections_count();
    res.outgoing_connections_count = restricted ? 0 : m_p2p.get_public_outgoing_connections_count();
    res.incoming_connections_count = restricted ? 0 : (total_conn - res.outgoing_connections_count);
    // FIXME: We don't really have RPC connections here anymore, and HTTP/LMQ RPC interfaces
    // deliberately sit outside this.  Deprecate it for now since there's no trivial way to get it,
    // but it might be useful to bring it back.
    //res.rpc_connections_count = restricted ? 0 : get_connections_count();
    res.white_peerlist_size = restricted ? 0 : m_p2p.get_public_white_peers_count();
    res.grey_peerlist_size = restricted ? 0 : m_p2p.get_public_gray_peers_count();

    cryptonote::network_type nettype = m_core.get_nettype();
    res.mainnet = nettype == MAINNET;
    res.testnet = nettype == TESTNET;
    res.stagenet = nettype == STAGENET;
    res.nettype = nettype == MAINNET ? "mainnet" : nettype == TESTNET ? "testnet" : nettype == STAGENET ? "stagenet" : "fakechain";

    try
    {
      res.cumulative_difficulty = m_core.get_blockchain_storage().get_db().get_block_cumulative_difficulty(res.height - 1);
    }
    catch(std::exception const &e)
    {
      res.status = "Error retrieving cumulative difficulty at height " + std::to_string(res.height - 1);
      return res;
    }

    res.service_node = m_core.service_node();
    res.block_size_limit = res.block_weight_limit = m_core.get_blockchain_storage().get_current_cumulative_block_weight_limit();
    res.block_size_median = res.block_weight_median = m_core.get_blockchain_storage().get_current_cumulative_block_weight_median();
    res.start_time = restricted ? 0 : (uint64_t)m_core.get_start_time();
    res.last_storage_server_ping = restricted ? 0 : (uint64_t)m_core.m_last_storage_server_ping;
    res.last_sispopnet_ping = restricted ? 0 : (uint64_t)m_core.m_last_sispopnet_ping;
    res.free_space = restricted ? std::numeric_limits<uint64_t>::max() : m_core.get_free_space();
    res.offline = m_core.offline();
    res.height_without_bootstrap = restricted ? 0 : res.height;
    if (restricted)
    {
      res.bootstrap_daemon_address = "";
      res.was_bootstrap_ever_used = false;
    }
    else
    {
      std::shared_lock lock{m_bootstrap_daemon_mutex};
      if (m_bootstrap_daemon.get() != nullptr)
      {
        res.bootstrap_daemon_address = m_bootstrap_daemon->address();
      }
      res.was_bootstrap_ever_used = m_was_bootstrap_ever_used;
    }
    res.database_size = m_core.get_blockchain_storage().get_db().get_database_size();
    if (restricted)
      res.database_size = round_up(res.database_size, 1'000'000'000);
    res.update_available = restricted ? false : m_core.is_update_available();
    res.version = restricted ? std::to_string(SISPOP_VERSION[0]) : SISPOP_VERSION_FULL;
    res.status_line = !restricted ? m_core.get_status_string() :
      "v" + std::to_string(SISPOP_VERSION[0]) + "; Height: " + std::to_string(res.height);

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_NET_STATS::response core_rpc_server::invoke(GET_NET_STATS::request&& req, rpc_context context)
  {
    GET_NET_STATS::response res{};

    PERF_TIMER(on_get_net_stats);
    // No bootstrap daemon check: Only ever get stats about local server
    res.start_time = (uint64_t)m_core.get_start_time();
    {
      std::lock_guard lock{epee::net_utils::network_throttle_manager::m_lock_get_global_throttle_in};
      epee::net_utils::network_throttle_manager::get_global_throttle_in().get_stats(res.total_packets_in, res.total_bytes_in);
    }
    {
      std::lock_guard lock{epee::net_utils::network_throttle_manager::m_lock_get_global_throttle_out};
      epee::net_utils::network_throttle_manager::get_global_throttle_out().get_stats(res.total_packets_out, res.total_bytes_out);
    }
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  class pruned_transaction {
    transaction& tx;
  public:
    pruned_transaction(transaction& tx) : tx(tx) {}
    BEGIN_SERIALIZE_OBJECT()
      tx.serialize_base(ar);
    END_SERIALIZE()
  };
  //------------------------------------------------------------------------------------------------------------------------------
  GET_BLOCKS_FAST::response core_rpc_server::invoke(GET_BLOCKS_FAST::request&& req, rpc_context context)
  {
    GET_BLOCKS_FAST::response res{};

    PERF_TIMER(on_get_blocks);
    if (use_bootstrap_daemon_if_necessary<GET_BLOCKS_FAST>(req, res))
      return res;

    std::vector<std::pair<std::pair<cryptonote::blobdata, crypto::hash>, std::vector<std::pair<crypto::hash, cryptonote::blobdata> > > > bs;

    if(!m_core.find_blockchain_supplement(req.start_height, req.block_ids, bs, res.current_height, res.start_height, req.prune, !req.no_miner_tx, GET_BLOCKS_FAST::MAX_COUNT))
    {
      res.status = "Failed";
      return res;
    }

    size_t size = 0, ntxes = 0;
    res.blocks.reserve(bs.size());
    res.output_indices.reserve(bs.size());
    for(auto& bd: bs)
    {
      res.blocks.resize(res.blocks.size()+1);
      res.blocks.back().block = bd.first.first;
      size += bd.first.first.size();
      res.output_indices.push_back(GET_BLOCKS_FAST::block_output_indices());
      ntxes += bd.second.size();
      res.output_indices.back().indices.reserve(1 + bd.second.size());
      if (req.no_miner_tx)
        res.output_indices.back().indices.push_back(GET_BLOCKS_FAST::tx_output_indices());
      res.blocks.back().txs.reserve(bd.second.size());
      for (std::vector<std::pair<crypto::hash, cryptonote::blobdata>>::iterator i = bd.second.begin(); i != bd.second.end(); ++i)
      {
        res.blocks.back().txs.push_back({std::move(i->second), crypto::null_hash});
        i->second.clear();
        i->second.shrink_to_fit();
        size += res.blocks.back().txs.back().size();
      }

      const size_t n_txes_to_lookup = bd.second.size() + (req.no_miner_tx ? 0 : 1);
      if (n_txes_to_lookup > 0)
      {
        std::vector<std::vector<uint64_t>> indices;
        bool r = m_core.get_tx_outputs_gindexs(req.no_miner_tx ? bd.second.front().first : bd.first.second, n_txes_to_lookup, indices);
        if (!r || indices.size() != n_txes_to_lookup || res.output_indices.back().indices.size() != (req.no_miner_tx ? 1 : 0))
        {
          res.status = "Failed";
          return res;
        }
        for (size_t i = 0; i < indices.size(); ++i)
          res.output_indices.back().indices.push_back({std::move(indices[i])});
      }
    }

    MDEBUG("on_get_blocks: " << bs.size() << " blocks, " << ntxes << " txes, size " << size);
    res.status = STATUS_OK;
    return res;
  }
  GET_ALT_BLOCKS_HASHES::response core_rpc_server::invoke(GET_ALT_BLOCKS_HASHES::request&& req, rpc_context context)
  {
    GET_ALT_BLOCKS_HASHES::response res{};

    PERF_TIMER(on_get_alt_blocks_hashes);
    if (use_bootstrap_daemon_if_necessary<GET_ALT_BLOCKS_HASHES>(req, res))
      return res;

    std::vector<block> blks;

    if(!m_core.get_alternative_blocks(blks))
    {
        res.status = "Failed";
        return res;
    }

    res.blks_hashes.reserve(blks.size());

    for (auto const& blk: blks)
    {
        res.blks_hashes.push_back(epee::string_tools::pod_to_hex(get_block_hash(blk)));
    }

    MDEBUG("on_get_alt_blocks_hashes: " << blks.size() << " blocks " );
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_BLOCKS_BY_HEIGHT::response core_rpc_server::invoke(GET_BLOCKS_BY_HEIGHT::request&& req, rpc_context context)
  {
    GET_BLOCKS_BY_HEIGHT::response res{};

    PERF_TIMER(on_get_blocks_by_height);
    if (use_bootstrap_daemon_if_necessary<GET_BLOCKS_BY_HEIGHT>(req, res))
      return res;

    res.status = "Failed";
    res.blocks.clear();
    res.blocks.reserve(req.heights.size());
    for (uint64_t height : req.heights)
    {
      block blk;
      try
      {
        blk = m_core.get_blockchain_storage().get_db().get_block_from_height(height);
      }
      catch (...)
      {
        res.status = "Error retrieving block at height " + std::to_string(height);
        return res;
      }
      std::vector<transaction> txs;
      std::vector<crypto::hash> missed_txs;
      m_core.get_transactions(blk.tx_hashes, txs, missed_txs);
      res.blocks.resize(res.blocks.size() + 1);
      res.blocks.back().block = block_to_blob(blk);
      for (auto& tx : txs)
        res.blocks.back().txs.push_back(tx_to_blob(tx));
    }
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_HASHES_FAST::response core_rpc_server::invoke(GET_HASHES_FAST::request&& req, rpc_context context)
  {
    GET_HASHES_FAST::response res{};

    PERF_TIMER(on_get_hashes);
    if (use_bootstrap_daemon_if_necessary<GET_HASHES_FAST>(req, res))
      return res;

    res.start_height = req.start_height;
    if(!m_core.get_blockchain_storage().find_blockchain_supplement(req.block_ids, res.m_block_ids, res.start_height, res.current_height, false))
    {
      res.status = "Failed";
      return res;
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_OUTPUTS_BIN::response core_rpc_server::invoke(GET_OUTPUTS_BIN::request&& req, rpc_context context)
  {
    GET_OUTPUTS_BIN::response res{};

    PERF_TIMER(on_get_outs_bin);
    if (use_bootstrap_daemon_if_necessary<GET_OUTPUTS_BIN>(req, res))
      return res;

    if (!context.admin && req.outputs.size() > MAX_RESTRICTED_GLOBAL_FAKE_OUTS_COUNT)
      res.status = "Too many outs requested";
    else if (m_core.get_outs(req, res))
      res.status = STATUS_OK;
    else
      res.status = "Failed";

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_OUTPUTS::response core_rpc_server::invoke(GET_OUTPUTS::request&& req, rpc_context context)
  {
    GET_OUTPUTS::response res{};

    PERF_TIMER(on_get_outs);
    if (use_bootstrap_daemon_if_necessary<GET_OUTPUTS>(req, res))
      return res;

    if (!context.admin && req.outputs.size() > MAX_RESTRICTED_GLOBAL_FAKE_OUTS_COUNT) {
      res.status = "Too many outs requested";
      return res;
    }

    GET_OUTPUTS_BIN::request req_bin{};
    req_bin.outputs = req.outputs;
    req_bin.get_txid = req.get_txid;
    GET_OUTPUTS_BIN::response res_bin{};
    if (!m_core.get_outs(req_bin, res_bin))
    {
      res.status = "Failed";
      return res;
    }

    // convert to text
    for (const auto &i: res_bin.outs)
    {
      res.outs.emplace_back();
      auto& outkey = res.outs.back();
      outkey.key = epee::string_tools::pod_to_hex(i.key);
      outkey.mask = epee::string_tools::pod_to_hex(i.mask);
      outkey.unlocked = i.unlocked;
      outkey.height = i.height;
      outkey.txid = epee::string_tools::pod_to_hex(i.txid);
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_TX_GLOBAL_OUTPUTS_INDEXES::response core_rpc_server::invoke(GET_TX_GLOBAL_OUTPUTS_INDEXES::request&& req, rpc_context context)
  {
    GET_TX_GLOBAL_OUTPUTS_INDEXES::response res{};

    PERF_TIMER(on_get_indexes);
    if (use_bootstrap_daemon_if_necessary<GET_TX_GLOBAL_OUTPUTS_INDEXES>(req, res))
      return res;

    bool r = m_core.get_tx_outputs_gindexs(req.txid, res.o_indexes);
    if(!r)
    {
      res.status = "Failed";
      return res;
    }
    res.status = STATUS_OK;
    LOG_PRINT_L2("GET_TX_GLOBAL_OUTPUTS_INDEXES: [" << res.o_indexes.size() << "]");
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_TRANSACTIONS::response core_rpc_server::invoke(GET_TRANSACTIONS::request&& req, rpc_context context)
  {
    GET_TRANSACTIONS::response res{};

    PERF_TIMER(on_get_transactions);
    if (use_bootstrap_daemon_if_necessary<GET_TRANSACTIONS>(req, res))
      return res;

    std::vector<crypto::hash> vh;
    for(const auto& tx_hex_str: req.txs_hashes)
    {
      blobdata b;
      if(!string_tools::parse_hexstr_to_binbuff(tx_hex_str, b))
      {
        res.status = "Failed to parse hex representation of transaction hash";
        return res;
      }
      if(b.size() != sizeof(crypto::hash))
      {
        res.status = "Failed, size of data mismatch";
        return res;
      }
      vh.push_back(*reinterpret_cast<const crypto::hash*>(b.data()));
    }
    std::vector<crypto::hash> missed_txs;
    std::vector<std::tuple<crypto::hash, cryptonote::blobdata, crypto::hash, cryptonote::blobdata>> txs;
    bool r = m_core.get_split_transactions_blobs(vh, txs, missed_txs);
    if(!r)
    {
      res.status = "Failed";
      return res;
    }
    LOG_PRINT_L2("Found " << txs.size() << "/" << vh.size() << " transactions on the blockchain");

    // try the pool for any missing txes
    auto &pool = m_core.get_pool();
    size_t found_in_pool = 0;
    std::unordered_map<crypto::hash, tx_info> per_tx_pool_tx_info;
    if (!missed_txs.empty())
    {
      std::vector<tx_info> pool_tx_info;
      std::vector<spent_key_image_info> pool_key_image_info;
      bool r = pool.get_transactions_and_spent_keys_info(pool_tx_info, pool_key_image_info, context.admin);
      if(r)
      {
        // sort to match original request
        std::vector<std::tuple<crypto::hash, cryptonote::blobdata, crypto::hash, cryptonote::blobdata>> sorted_txs;
        unsigned txs_processed = 0;
        for (const crypto::hash &h: vh)
        {
          auto missed_it = std::find(missed_txs.begin(), missed_txs.end(), h);
          if (missed_it == missed_txs.end())
          {
            if (txs.size() == txs_processed)
            {
              res.status = "Failed: internal error - txs is empty";
              return res;
            }
            // core returns the ones it finds in the right order
            if (std::get<0>(txs[txs_processed]) != h)
            {
              res.status = "Failed: tx hash mismatch";
              return res;
            }
            sorted_txs.push_back(std::move(txs[txs_processed]));
            ++txs_processed;
            continue;
          }
          const std::string hash_string = epee::string_tools::pod_to_hex(h);
          auto ptx_it = std::find_if(pool_tx_info.begin(), pool_tx_info.end(),
              [&hash_string](const tx_info &txi) { return hash_string == txi.id_hash; });
          if (ptx_it != pool_tx_info.end())
          {
            cryptonote::transaction tx;
            if (!cryptonote::parse_and_validate_tx_from_blob(ptx_it->tx_blob, tx))
            {
              res.status = "Failed to parse and validate tx from blob";
              return res;
            }
            serialization::binary_string_archiver ba;
            try {
              tx.serialize_base(ba);
            } catch (const std::exception& e) {
              res.status = "Failed to serialize transaction base: "s + e.what();
              return res;
            }
            std::string pruned = ba.str();
            std::string pruned2{ptx_it->tx_blob, pruned.size()};
            sorted_txs.emplace_back(h, std::move(pruned), get_transaction_prunable_hash(tx), std::move(pruned2));
            missed_txs.erase(missed_it);
            per_tx_pool_tx_info.emplace(h, *ptx_it);
            ++found_in_pool;
          }
        }
        txs = sorted_txs;
      }
      LOG_PRINT_L2("Found " << found_in_pool << "/" << vh.size() << " transactions in the pool");
    }

    uint64_t immutable_height = m_core.get_blockchain_storage().get_immutable_height();
    auto blink_lock = pool.blink_shared_lock(std::defer_lock); // Defer until/unless we actually need it

    std::vector<std::string>::const_iterator txhi = req.txs_hashes.begin();
    std::vector<crypto::hash>::const_iterator vhi = vh.begin();
    for(auto& tx: txs)
    {
      res.txs.emplace_back();
      GET_TRANSACTIONS::entry &e = res.txs.back();

      crypto::hash tx_hash = *vhi++;
      e.tx_hash = *txhi++;
      e.prunable_hash = epee::string_tools::pod_to_hex(std::get<2>(tx));
      if (req.split || req.prune || std::get<3>(tx).empty())
      {
        // use splitted form with pruned and prunable (filled only when prune=false and the daemon has it), leaving as_hex as empty
        e.pruned_as_hex = string_tools::buff_to_hex_nodelimer(std::get<1>(tx));
        if (!req.prune)
          e.prunable_as_hex = string_tools::buff_to_hex_nodelimer(std::get<3>(tx));
        if (req.decode_as_json)
        {
          cryptonote::blobdata tx_data;
          cryptonote::transaction t;
          if (req.prune || std::get<3>(tx).empty())
          {
            // decode pruned tx to JSON
            tx_data = std::get<1>(tx);
            if (cryptonote::parse_and_validate_tx_base_from_blob(tx_data, t))
            {
              pruned_transaction pruned_tx{t};
              e.as_json = obj_to_json_str(pruned_tx);
            }
            else
            {
              res.status = "Failed to parse and validate pruned tx from blob";
              return res;
            }
          }
          else
          {
            // decode full tx to JSON
            tx_data = std::get<1>(tx) + std::get<3>(tx);
            if (cryptonote::parse_and_validate_tx_from_blob(tx_data, t))
            {
              e.as_json = obj_to_json_str(t);
            }
            else
            {
              res.status = "Failed to parse and validate tx from blob";
              return res;
            }
          }
        }
      }
      else
      {
        // use non-splitted form, leaving pruned_as_hex and prunable_as_hex as empty
        cryptonote::blobdata tx_data = std::get<1>(tx) + std::get<3>(tx);
        e.as_hex = string_tools::buff_to_hex_nodelimer(tx_data);
        if (req.decode_as_json)
        {
          cryptonote::transaction t;
          if (cryptonote::parse_and_validate_tx_from_blob(tx_data, t))
          {
            e.as_json = obj_to_json_str(t);
          }
          else
          {
            res.status = "Failed to parse and validate tx from blob";
            return res;
          }
        }
      }
      auto ptx_it = per_tx_pool_tx_info.find(tx_hash);
      e.in_pool = ptx_it != per_tx_pool_tx_info.end();
      bool might_be_blink = true;
      if (e.in_pool)
      {
        e.block_height = e.block_timestamp = std::numeric_limits<uint64_t>::max();
        e.double_spend_seen = ptx_it->second.double_spend_seen;
        e.relayed = ptx_it->second.relayed;
        e.received_timestamp = ptx_it->second.receive_time;
      }
      else
      {
        e.block_height = m_core.get_blockchain_storage().get_db().get_tx_block_height(tx_hash);
        e.block_timestamp = m_core.get_blockchain_storage().get_db().get_block_timestamp(e.block_height);
        e.received_timestamp = 0;
        e.double_spend_seen = false;
        e.relayed = false;
        if (e.block_height <= immutable_height)
            might_be_blink = false;
      }

      if (might_be_blink)
      {
        if (!blink_lock) blink_lock.lock();
        e.blink = pool.has_blink(tx_hash);
      }

      // fill up old style responses too, in case an old wallet asks
      res.txs_as_hex.push_back(e.as_hex);
      if (req.decode_as_json)
        res.txs_as_json.push_back(e.as_json);

      // output indices too if not in pool
      if (!e.in_pool)
      {
        bool r = m_core.get_tx_outputs_gindexs(tx_hash, e.output_indices);
        if (!r)
        {
          res.status = "Failed";
          return res;
        }
      }
    }

    for(const auto& miss_tx: missed_txs)
    {
      res.missed_tx.push_back(string_tools::pod_to_hex(miss_tx));
    }

    LOG_PRINT_L2(res.txs.size() << " transactions found, " << res.missed_tx.size() << " not found");
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  IS_KEY_IMAGE_SPENT::response core_rpc_server::invoke(IS_KEY_IMAGE_SPENT::request&& req, rpc_context context)
  {
    IS_KEY_IMAGE_SPENT::response res{};

    PERF_TIMER(on_is_key_image_spent);
    if (use_bootstrap_daemon_if_necessary<IS_KEY_IMAGE_SPENT>(req, res))
      return res;

    std::vector<crypto::key_image> key_images;
    for(const auto& ki_hex_str: req.key_images)
    {
      blobdata b;
      if(!string_tools::parse_hexstr_to_binbuff(ki_hex_str, b))
      {
        res.status = "Failed to parse hex representation of key image";
        return res;
      }
      if(b.size() != sizeof(crypto::key_image))
      {
        res.status = "Failed, size of data mismatch";
      }
      key_images.push_back(*reinterpret_cast<const crypto::key_image*>(b.data()));
    }
    std::vector<bool> spent_status;
    bool r = m_core.are_key_images_spent(key_images, spent_status);
    if(!r)
    {
      res.status = "Failed";
      return res;
    }
    res.spent_status.clear();
    for (size_t n = 0; n < spent_status.size(); ++n)
      res.spent_status.push_back(spent_status[n] ? IS_KEY_IMAGE_SPENT::SPENT_IN_BLOCKCHAIN : IS_KEY_IMAGE_SPENT::UNSPENT);

    // check the pool too
    std::vector<tx_info> txs;
    std::vector<spent_key_image_info> ki;
    r = m_core.get_pool().get_transactions_and_spent_keys_info(txs, ki, context.admin);
    if(!r)
    {
      res.status = "Failed";
      return res;
    }
    for (std::vector<spent_key_image_info>::const_iterator i = ki.begin(); i != ki.end(); ++i)
    {
      crypto::hash hash;
      crypto::key_image spent_key_image;
      if (parse_hash256(i->id_hash, hash))
      {
        memcpy(&spent_key_image, &hash, sizeof(hash)); // a bit dodgy, should be other parse functions somewhere
        for (size_t n = 0; n < res.spent_status.size(); ++n)
        {
          if (res.spent_status[n] == IS_KEY_IMAGE_SPENT::UNSPENT)
          {
            if (key_images[n] == spent_key_image)
            {
              res.spent_status[n] = IS_KEY_IMAGE_SPENT::SPENT_IN_POOL;
              break;
            }
          }
        }
      }
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SEND_RAW_TX::response core_rpc_server::invoke(SEND_RAW_TX::request&& req, rpc_context context)
  {
    SEND_RAW_TX::response res{};

    PERF_TIMER(on_send_raw_tx);
    if (use_bootstrap_daemon_if_necessary<SEND_RAW_TX>(req, res))
      return res;

    CHECK_CORE_READY();

    std::string tx_blob;
    if(!string_tools::parse_hexstr_to_binbuff(req.tx_as_hex, tx_blob))
    {
      LOG_PRINT_L0("[on_send_raw_tx]: Failed to parse tx from hexbuff: " << req.tx_as_hex);
      res.status = "Failed";
      return res;
    }

    if (req.do_sanity_checks && !cryptonote::tx_sanity_check(tx_blob, m_core.get_blockchain_storage().get_num_mature_outputs(0)))
    {
      res.status = "Failed";
      res.reason = "Sanity check failed";
      res.sanity_check_failed = true;
      return res;
    }
    res.sanity_check_failed = false;

    if (req.blink)
    {
      auto future = m_core.handle_blink_tx(tx_blob);
      auto status = future.wait_for(10s);
      if (status != std::future_status::ready) {
        res.status = "Failed";
        res.reason = "Blink quorum timeout";
        res.blink_status = blink_result::timeout;
        return res;
      }

      try {
        auto result = future.get();
        res.blink_status = result.first;
        if (result.first == blink_result::accepted) {
          res.status = STATUS_OK;
        } else {
          res.status = "Failed";
          res.reason = !result.second.empty() ? result.second : result.first == blink_result::timeout ? "Blink quorum timeout" : "Transaction rejected by blink quorum";
        }
      } catch (const std::exception &e) {
        res.blink_status = blink_result::rejected;
        res.status = "Failed";
        res.reason = std::string{"Transaction failed: "} + e.what();
      }
      return res;
    }

    tx_verification_context tvc{};
    if(!m_core.handle_incoming_tx(tx_blob, tvc, tx_pool_options::new_tx(req.do_not_relay)) || tvc.m_verifivation_failed)
    {
      const vote_verification_context &vvc = tvc.m_vote_ctx;
      res.status          = "Failed";
      std::string reason  = print_tx_verification_context  (tvc);
      reason             += print_vote_verification_context(vvc);
      res.tvc             = tvc;
      const std::string punctuation = res.reason.empty() ? "" : ": ";
      if (tvc.m_verifivation_failed)
      {
        LOG_PRINT_L0("[on_send_raw_tx]: tx verification failed" << punctuation << reason);
      }
      else
      {
        LOG_PRINT_L0("[on_send_raw_tx]: Failed to process tx" << punctuation << reason);
      }
      return res;
    }

    if(!tvc.m_should_be_relayed)
    {
      LOG_PRINT_L0("[on_send_raw_tx]: tx accepted, but not relayed");
      res.reason = "Not relayed";
      res.not_relayed = true;
      res.status = STATUS_OK;
      return res;
    }

    NOTIFY_NEW_TRANSACTIONS::request r{};
    r.txs.push_back(tx_blob);
    cryptonote_connection_context fake_context{};
    m_core.get_protocol()->relay_transactions(r, fake_context);

    //TODO: make sure that tx has reached other nodes here, probably wait to receive reflections from other nodes
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  START_MINING::response core_rpc_server::invoke(START_MINING::request&& req, rpc_context context)
  {
    START_MINING::response res{};

    PERF_TIMER(on_start_mining);
    CHECK_CORE_READY();
    cryptonote::address_parse_info info;
    if(!get_account_address_from_str(info, m_core.get_nettype(), req.miner_address))
    {
      res.status = "Failed, wrong address";
      LOG_PRINT_L0(res.status);
      return res;
    }
    if (info.is_subaddress)
    {
      res.status = "Mining to subaddress isn't supported yet";
      LOG_PRINT_L0(res.status);
      return res;
    }

    unsigned int concurrency_count = std::thread::hardware_concurrency() * 4;

    // if we couldn't detect threads, set it to a ridiculously high number
    if(concurrency_count == 0)
    {
      concurrency_count = 257;
    }

    // if there are more threads requested than the hardware supports
    // then we fail and log that.
    if(req.threads_count > concurrency_count)
    {
      res.status = "Failed, too many threads relative to CPU cores.";
      LOG_PRINT_L0(res.status);
      return res;
    }

    cryptonote::miner &miner= m_core.get_miner();
    if (miner.is_mining())
    {
      res.status = "Already mining";
      return res;
    }
    if(!miner.start(info.address, static_cast<size_t>(req.threads_count)))
    {
      res.status = "Failed, mining not started";
      LOG_PRINT_L0(res.status);
      return res;
    }
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  STOP_MINING::response core_rpc_server::invoke(STOP_MINING::request&& req, rpc_context context)
  {
    STOP_MINING::response res{};

    PERF_TIMER(on_stop_mining);
    cryptonote::miner &miner= m_core.get_miner();
    if(!miner.is_mining())
    {
      res.status = "Mining never started";
      LOG_PRINT_L0(res.status);
      return res;
    }
    if(!miner.stop())
    {
      res.status = "Failed, mining not stopped";
      LOG_PRINT_L0(res.status);
      return res;
    }
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  MINING_STATUS::response core_rpc_server::invoke(MINING_STATUS::request&& req, rpc_context context)
  {
    MINING_STATUS::response res{};

    PERF_TIMER(on_mining_status);

    const miner& lMiner = m_core.get_miner();
    res.active = lMiner.is_mining();
    res.block_target = DIFFICULTY_TARGET_V2;
    res.difficulty = m_core.get_blockchain_storage().get_difficulty_for_next_block();
    if ( lMiner.is_mining() ) {
      res.speed = lMiner.get_speed();
      res.threads_count = lMiner.get_threads_count();
      res.block_reward = lMiner.get_block_reward();
    }
    const account_public_address& lMiningAdr = lMiner.get_mining_address();
    if (lMiner.is_mining())
      res.address = get_account_address_as_str(nettype(), false, lMiningAdr);
    const uint8_t major_version = m_core.get_blockchain_storage().get_current_hard_fork_version();

    res.pow_algorithm =
        major_version >= network_version_12_checkpointing    ? "RandomX (SISPOP variant)"               :
        major_version == network_version_11_infinite_staking ? "Cryptonight Turtle Light (Variant 2)" :
                                                               "Cryptonight Heavy (Variant 2)";

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SAVE_BC::response core_rpc_server::invoke(SAVE_BC::request&& req, rpc_context context)
  {
    SAVE_BC::response res{};

    PERF_TIMER(on_save_bc);
    if( !m_core.get_blockchain_storage().store_blockchain() )
    {
      res.status = "Error while storing blockchain";
      return res;
    }
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_PEER_LIST::response core_rpc_server::invoke(GET_PEER_LIST::request&& req, rpc_context context)
  {
    GET_PEER_LIST::response res{};

    PERF_TIMER(on_get_peer_list);
    std::vector<nodetool::peerlist_entry> white_list;
    std::vector<nodetool::peerlist_entry> gray_list;

    if (req.public_only)
    {
      m_p2p.get_public_peerlist(gray_list, white_list);
    }
    else
    {
      m_p2p.get_peerlist(gray_list, white_list);
    }

    for (auto & entry : white_list)
    {
      if (entry.adr.get_type_id() == epee::net_utils::ipv4_network_address::get_type_id())
        res.white_list.emplace_back(entry.id, entry.adr.as<epee::net_utils::ipv4_network_address>().ip(),
            entry.adr.as<epee::net_utils::ipv4_network_address>().port(), entry.last_seen, entry.pruning_seed, entry.rpc_port);
      else if (entry.adr.get_type_id() == epee::net_utils::ipv6_network_address::get_type_id())
        res.white_list.emplace_back(entry.id, entry.adr.as<epee::net_utils::ipv6_network_address>().host_str(),
            entry.adr.as<epee::net_utils::ipv6_network_address>().port(), entry.last_seen, entry.pruning_seed, entry.rpc_port);
      else
        res.white_list.emplace_back(entry.id, entry.adr.str(), entry.last_seen, entry.pruning_seed, entry.rpc_port);
    }

    for (auto & entry : gray_list)
    {
      if (entry.adr.get_type_id() == epee::net_utils::ipv4_network_address::get_type_id())
        res.gray_list.emplace_back(entry.id, entry.adr.as<epee::net_utils::ipv4_network_address>().ip(),
            entry.adr.as<epee::net_utils::ipv4_network_address>().port(), entry.last_seen, entry.pruning_seed, entry.rpc_port);
      else if (entry.adr.get_type_id() == epee::net_utils::ipv6_network_address::get_type_id())
        res.gray_list.emplace_back(entry.id, entry.adr.as<epee::net_utils::ipv6_network_address>().host_str(),
            entry.adr.as<epee::net_utils::ipv6_network_address>().port(), entry.last_seen, entry.pruning_seed, entry.rpc_port);
      else
        res.gray_list.emplace_back(entry.id, entry.adr.str(), entry.last_seen, entry.pruning_seed, entry.rpc_port);
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_PUBLIC_NODES::response core_rpc_server::invoke(GET_PUBLIC_NODES::request&& req, rpc_context context)
  {
    PERF_TIMER(on_get_public_nodes);

    GET_PEER_LIST::response peer_list_res = invoke(GET_PEER_LIST::request{}, context);
    GET_PUBLIC_NODES::response res{};
    res.status = std::move(peer_list_res.status);

    const auto collect = [](const std::vector<GET_PEER_LIST::peer> &peer_list, std::vector<public_node> &public_nodes)
    {
      for (const auto &entry : peer_list)
      {
        if (entry.rpc_port != 0)
        {
          public_nodes.emplace_back(entry);
        }
      }
    };

    if (req.white)
    {
      collect(peer_list_res.white_list, res.white);
    }
    if (req.gray)
    {
      collect(peer_list_res.gray_list, res.gray);
    }

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SET_LOG_HASH_RATE::response core_rpc_server::invoke(SET_LOG_HASH_RATE::request&& req, rpc_context context)
  {
    SET_LOG_HASH_RATE::response res{};

    PERF_TIMER(on_set_log_hash_rate);
    if(m_core.get_miner().is_mining())
    {
      m_core.get_miner().do_print_hashrate(req.visible);
      res.status = STATUS_OK;
    }
    else
    {
      res.status = STATUS_NOT_MINING;
    }
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SET_LOG_LEVEL::response core_rpc_server::invoke(SET_LOG_LEVEL::request&& req, rpc_context context)
  {
    SET_LOG_LEVEL::response res{};

    PERF_TIMER(on_set_log_level);
    if (req.level < 0 || req.level > 4)
    {
      res.status = "Error: log level not valid";
      return res;
    }
    mlog_set_log_level(req.level);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SET_LOG_CATEGORIES::response core_rpc_server::invoke(SET_LOG_CATEGORIES::request&& req, rpc_context context)
  {
    SET_LOG_CATEGORIES::response res{};

    PERF_TIMER(on_set_log_categories);
    mlog_set_log(req.categories.c_str());
    res.categories = mlog_get_categories();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_TRANSACTION_POOL::response core_rpc_server::invoke(GET_TRANSACTION_POOL::request&& req, rpc_context context)
  {
    GET_TRANSACTION_POOL::response res{};

    PERF_TIMER(on_get_transaction_pool);
    if (use_bootstrap_daemon_if_necessary<GET_TRANSACTION_POOL>(req, res))
      return res;

    m_core.get_pool().get_transactions_and_spent_keys_info(res.transactions, res.spent_key_images, context.admin);
    for (tx_info& txi : res.transactions)
      txi.tx_blob = epee::string_tools::buff_to_hex_nodelimer(txi.tx_blob);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_TRANSACTION_POOL_HASHES_BIN::response core_rpc_server::invoke(GET_TRANSACTION_POOL_HASHES_BIN::request&& req, rpc_context context)
  {
    GET_TRANSACTION_POOL_HASHES_BIN::response res{};

    PERF_TIMER(on_get_transaction_pool_hashes);
    if (use_bootstrap_daemon_if_necessary<GET_TRANSACTION_POOL_HASHES_BIN>(req, res))
      return res;

    std::vector<crypto::hash> tx_pool_hashes;
    m_core.get_pool().get_transaction_hashes(tx_pool_hashes, context.admin);

    if (req.long_poll)
    {
      /** FIXME: this needs to go into HTTP RPC-specific layer
       *
      if (m_max_long_poll_connections <= 0)
      {
        // Essentially disable long polling by making the wallet long polling thread go to sleep due to receiving this message
        res.status = STATUS_TX_LONG_POLL_MAX_CONNECTIONS;
        return res;
      }

      crypto::hash checksum = {};
      for (crypto::hash const &hash : tx_pool_hashes) crypto::hash_xor(checksum, hash);

      if (req.tx_pool_checksum == checksum)
      {
        size_t tx_count_before = tx_pool_hashes.size();
        time_t before          = time(nullptr);
        std::unique_lock<std::mutex> lock(m_core.m_long_poll_mutex);
        if ((m_long_poll_active_connections + 1) > m_max_long_poll_connections)
        {
          res.status = STATUS_TX_LONG_POLL_MAX_CONNECTIONS;
          return res;
        }

        m_long_poll_active_connections++;
        bool condition_activated = m_core.m_long_poll_wake_up_clients.wait_for(lock, long_poll_timeout, [this, tx_count_before]() {
              size_t tx_count_after = m_core.get_pool().get_transactions_count();
              return tx_count_before != tx_count_after;
            });

        m_long_poll_active_connections--;
        if (!condition_activated)
        {
          res.status = STATUS_TX_LONG_POLL_TIMED_OUT;
          return res;
        }
      }
      */
    }

    res.tx_hashes = std::move(tx_pool_hashes);
    res.status    = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_TRANSACTION_POOL_HASHES::response core_rpc_server::invoke(GET_TRANSACTION_POOL_HASHES::request&& req, rpc_context context)
  {
    GET_TRANSACTION_POOL_HASHES::response res{};

    PERF_TIMER(on_get_transaction_pool_hashes);
    if (use_bootstrap_daemon_if_necessary<GET_TRANSACTION_POOL_HASHES>(req, res))
      return res;

    std::vector<crypto::hash> tx_hashes;
    m_core.get_pool().get_transaction_hashes(tx_hashes, context.admin);
    res.tx_hashes.reserve(tx_hashes.size());
    for (const crypto::hash &tx_hash: tx_hashes)
      res.tx_hashes.push_back(epee::string_tools::pod_to_hex(tx_hash));
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_TRANSACTION_POOL_STATS::response core_rpc_server::invoke(GET_TRANSACTION_POOL_STATS::request&& req, rpc_context context)
  {
    GET_TRANSACTION_POOL_STATS::response res{};

    PERF_TIMER(on_get_transaction_pool_stats);
    if (use_bootstrap_daemon_if_necessary<GET_TRANSACTION_POOL_STATS>(req, res))
      return res;

    m_core.get_pool().get_transaction_stats(res.pool_stats, context.admin);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SET_BOOTSTRAP_DAEMON::response core_rpc_server::invoke(SET_BOOTSTRAP_DAEMON::request&& req, rpc_context context)
  {
    PERF_TIMER(on_set_bootstrap_daemon);

    std::optional<epee::net_utils::http::login> credentials;
    if (!req.username.empty() || !req.password.empty())
    {
      credentials = epee::net_utils::http::login(req.username, req.password);
    }

    if (!set_bootstrap_daemon(req.address, credentials))
      throw rpc_error{ERROR_WRONG_PARAM, "Failed to set bootstrap daemon to address = " + req.address};

    SET_BOOTSTRAP_DAEMON::response res{};
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  STOP_DAEMON::response core_rpc_server::invoke(STOP_DAEMON::request&& req, rpc_context context)
  {
    STOP_DAEMON::response res{};

    PERF_TIMER(on_stop_daemon);
    m_p2p.send_stop_signal();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------

  //
  // Sispop
  //
  GET_OUTPUT_BLACKLIST::response core_rpc_server::invoke(GET_OUTPUT_BLACKLIST::request&& req, rpc_context context)
  {
    GET_OUTPUT_BLACKLIST::response res{};

    PERF_TIMER(on_get_output_blacklist_bin);

    if (use_bootstrap_daemon_if_necessary<GET_OUTPUT_BLACKLIST>(req, res))
      return res;

    try
    {
      m_core.get_output_blacklist(res.blacklist);
    }
    catch (const std::exception &e)
    {
      res.status = std::string("Failed to get output blacklist: ") + e.what();
      return res;
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GETBLOCKCOUNT::response core_rpc_server::invoke(GETBLOCKCOUNT::request&& req, rpc_context context)
  {
    GETBLOCKCOUNT::response res{};

    PERF_TIMER(on_getblockcount);
    {
      std::shared_lock lock{m_bootstrap_daemon_mutex};
      if (m_should_use_bootstrap_daemon)
      {
        res.status = "This command is unsupported for bootstrap daemon";
        return res;
      }
    }
    res.count = m_core.get_current_blockchain_height();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GETBLOCKHASH::response core_rpc_server::invoke(GETBLOCKHASH::request&& req, rpc_context context)
  {
    GETBLOCKHASH::response res{};

    PERF_TIMER(on_getblockhash);
    {
      std::shared_lock lock{m_bootstrap_daemon_mutex};
      if (m_should_use_bootstrap_daemon)
      {
        res = "This command is unsupported for bootstrap daemon";
        return res;
      }
    }
    if(req.height.size() != 1)
      throw rpc_error{ERROR_WRONG_PARAM, "Wrong parameters, expected height"};

    uint64_t h = req.height[0];
    if(m_core.get_current_blockchain_height() <= h)
      throw rpc_error{ERROR_TOO_BIG_HEIGHT,
        "Requested block height: " + std::to_string(h) + " greater than current top block height: " +  std::to_string(m_core.get_current_blockchain_height() - 1)};

    res = string_tools::pod_to_hex(m_core.get_block_id_by_height(h));
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GETBLOCKTEMPLATE::response core_rpc_server::invoke(GETBLOCKTEMPLATE::request&& req, rpc_context context)
  {
    GETBLOCKTEMPLATE::response res{};

    PERF_TIMER(on_getblocktemplate);
    if (use_bootstrap_daemon_if_necessary<GETBLOCKTEMPLATE>(req, res))
      return res;

    if(!check_core_ready())
      throw rpc_error{ERROR_CORE_BUSY, "Core is busy"};

    if(req.reserve_size > 255)
      throw rpc_error{ERROR_TOO_BIG_RESERVE_SIZE, "Too big reserved size, maximum 255"};

    if(req.reserve_size && !req.extra_nonce.empty())
      throw rpc_error{ERROR_WRONG_PARAM, "Cannot specify both a reserve_size and an extra_nonce"};

    if(req.extra_nonce.size() > 510)
      throw rpc_error{ERROR_TOO_BIG_RESERVE_SIZE, "Too big extra_nonce size, maximum 510 hex chars"};

    cryptonote::address_parse_info info;

    if(!req.wallet_address.size() || !cryptonote::get_account_address_from_str(info, m_core.get_nettype(), req.wallet_address))
      throw rpc_error{ERROR_WRONG_WALLET_ADDRESS, "Failed to parse wallet address"};
    if (info.is_subaddress)
      throw rpc_error{ERROR_MINING_TO_SUBADDRESS, "Mining to subaddress is not supported yet"};

    block b;
    cryptonote::blobdata blob_reserve;
    if(!req.extra_nonce.empty())
    {
      if(!string_tools::parse_hexstr_to_binbuff(req.extra_nonce, blob_reserve))
        throw rpc_error{ERROR_WRONG_PARAM, "Parameter extra_nonce should be a hex string"};
    }
    else
      blob_reserve.resize(req.reserve_size, 0);
    cryptonote::difficulty_type diff;
    crypto::hash prev_block;
    if (!req.prev_block.empty())
    {
      if (!epee::string_tools::hex_to_pod(req.prev_block, prev_block))
        throw rpc_error{ERROR_INTERNAL, "Invalid prev_block"};
    }
    if(!m_core.get_block_template(b, req.prev_block.empty() ? NULL : &prev_block, info.address, diff, res.height, res.expected_reward, blob_reserve))
    {
      LOG_ERROR("Failed to create block template");
      throw rpc_error{ERROR_INTERNAL, "Internal error: failed to create block template"};
    }

    if (b.major_version >= network_version_12_checkpointing)
    {
      uint64_t seed_height, next_height;
      crypto::hash seed_hash;
      crypto::rx_seedheights(res.height, &seed_height, &next_height);
      seed_hash = m_core.get_block_id_by_height(seed_height);
      res.seed_hash = string_tools::pod_to_hex(seed_hash);
      if (next_height != seed_height) {
        seed_hash = m_core.get_block_id_by_height(next_height);
        res.next_seed_hash = string_tools::pod_to_hex(seed_hash);
      }
    }
    res.difficulty = diff;

    blobdata block_blob = t_serializable_object_to_blob(b);
    crypto::public_key tx_pub_key = cryptonote::get_tx_pub_key_from_extra(b.miner_tx);
    if(tx_pub_key == crypto::null_pkey)
    {
      LOG_ERROR("Failed to get tx pub key in coinbase extra");
      throw rpc_error{ERROR_INTERNAL, "Internal error: failed to create block template"};
    }
    res.reserved_offset = block_blob.find(tx_pub_key.data, 0, sizeof(tx_pub_key.data));
    if (res.reserved_offset == block_blob.npos)
    {
      LOG_ERROR("Failed to find tx pub key in blockblob");
      throw rpc_error{ERROR_INTERNAL, "Internal error: failed to create block template"};
    }
    if (req.reserve_size)
      res.reserved_offset += sizeof(tx_pub_key) + 2; //2 bytes: tag for TX_EXTRA_NONCE(1 byte), counter in TX_EXTRA_NONCE(1 byte)
    else
      res.reserved_offset = 0;
    if(res.reserved_offset + req.reserve_size > block_blob.size())
    {
      LOG_ERROR("Failed to calculate offset for ");
      throw rpc_error{ERROR_INTERNAL, "Internal error: failed to create block template"};
    }
    blobdata hashing_blob = get_block_hashing_blob(b);
    res.prev_hash = string_tools::pod_to_hex(b.prev_id);
    res.blocktemplate_blob = string_tools::buff_to_hex_nodelimer(block_blob);
    res.blockhashing_blob =  string_tools::buff_to_hex_nodelimer(hashing_blob);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SUBMITBLOCK::response core_rpc_server::invoke(SUBMITBLOCK::request&& req, rpc_context context)
  {
    SUBMITBLOCK::response res{};

    PERF_TIMER(on_submitblock);
    {
      std::shared_lock lock{m_bootstrap_daemon_mutex};
      if (m_should_use_bootstrap_daemon)
      {
        res.status = "This command is unsupported for bootstrap daemon";
        return res;
      }
    }
    CHECK_CORE_READY();
    if(req.blob.size()!=1)
      throw rpc_error{ERROR_WRONG_PARAM, "Wrong param"};
    blobdata blockblob;
    if(!string_tools::parse_hexstr_to_binbuff(req.blob[0], blockblob))
      throw rpc_error{ERROR_WRONG_BLOCKBLOB, "Wrong block blob"};

    // Fixing of high orphan issue for most pools
    // Thanks Boolberry!
    block b;
    if(!parse_and_validate_block_from_blob(blockblob, b))
      throw rpc_error{ERROR_WRONG_BLOCKBLOB, "Wrong block blob"};

    // Fix from Boolberry neglects to check block
    // size, do that with the function below
    if(!m_core.check_incoming_block_size(blockblob))
      throw rpc_error{ERROR_WRONG_BLOCKBLOB_SIZE, "Block blob size is too big, rejecting block"};

    block_verification_context bvc;
    if(!m_core.handle_block_found(b, bvc))
      throw rpc_error{ERROR_BLOCK_NOT_ACCEPTED, "Block not accepted"};
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GENERATEBLOCKS::response core_rpc_server::invoke(GENERATEBLOCKS::request&& req, rpc_context context)
  {
    GENERATEBLOCKS::response res{};

    PERF_TIMER(on_generateblocks);

    CHECK_CORE_READY();

    res.status = STATUS_OK;

    if(m_core.get_nettype() != FAKECHAIN)
      throw rpc_error{ERROR_REGTEST_REQUIRED, "Regtest required when generating blocks"};

    SUBMITBLOCK::request submit_req{};
    submit_req.blob.emplace_back(); // string vector containing exactly one block blob

    res.height = m_core.get_blockchain_storage().get_current_blockchain_height();

    for(size_t i = 0; i < req.amount_of_blocks; i++)
    {
      GETBLOCKTEMPLATE::request template_req{};
      template_req.reserve_size = 1;
      template_req.wallet_address = req.wallet_address;
      template_req.prev_block = i == 0 ? req.prev_block : res.blocks.back();
      auto template_res = invoke(std::move(template_req), context);
      res.status = template_res.status;

      blobdata blockblob;
      if(!string_tools::parse_hexstr_to_binbuff(template_res.blocktemplate_blob, blockblob))
        throw rpc_error{ERROR_WRONG_BLOCKBLOB, "Wrong block blob"};
      block b;
      if(!parse_and_validate_block_from_blob(blockblob, b))
        throw rpc_error{ERROR_WRONG_BLOCKBLOB, "Wrong block blob"};
      b.nonce = req.starting_nonce;
      miner::find_nonce_for_given_block([this](const cryptonote::block &b, uint64_t height, unsigned int threads, crypto::hash &hash) {
        hash = cryptonote::get_block_longhash_w_blockchain(&(m_core.get_blockchain_storage()), b, height, threads);
        return true;
      }, b, template_res.difficulty, template_res.height);

      submit_req.blob[0] = string_tools::buff_to_hex_nodelimer(block_to_blob(b));
      auto submit_res = invoke(std::move(submit_req), context);
      res.status = submit_res.status;

      res.blocks.push_back(epee::string_tools::pod_to_hex(get_block_hash(b)));
      res.height = template_res.height;
    }

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  uint64_t core_rpc_server::get_block_reward(const block& blk)
  {
    uint64_t reward = 0;
    for(const tx_out& out: blk.miner_tx.vout)
    {
      reward += out.amount;
    }
    return reward;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  void core_rpc_server::fill_block_header_response(const block& blk, bool orphan_status, uint64_t height, const crypto::hash& hash, block_header_response& response, bool fill_pow_hash)
  {
    PERF_TIMER(fill_block_header_response);
    response.major_version = blk.major_version;
    response.minor_version = blk.minor_version;
    response.timestamp = blk.timestamp;
    response.prev_hash = string_tools::pod_to_hex(blk.prev_id);
    response.nonce = blk.nonce;
    response.orphan_status = orphan_status;
    response.height = height;
    response.depth = m_core.get_current_blockchain_height() - height - 1;
    response.hash = string_tools::pod_to_hex(hash);
    response.difficulty = m_core.get_blockchain_storage().block_difficulty(height);
    response.cumulative_difficulty = m_core.get_blockchain_storage().get_db().get_block_cumulative_difficulty(height);
    response.block_weight = m_core.get_blockchain_storage().get_db().get_block_weight(height);
    response.reward = get_block_reward(blk);
    response.miner_reward = blk.miner_tx.vout[0].amount;
    response.block_size = response.block_weight = m_core.get_blockchain_storage().get_db().get_block_weight(height);
    response.num_txes = blk.tx_hashes.size();
    response.pow_hash = fill_pow_hash ? string_tools::pod_to_hex(get_block_longhash_w_blockchain(&(m_core.get_blockchain_storage()), blk, height, 0)) : "";
    response.long_term_weight = m_core.get_blockchain_storage().get_db().get_block_long_term_weight(height);
    response.miner_tx_hash = string_tools::pod_to_hex(cryptonote::get_transaction_hash(blk.miner_tx));
    response.service_node_winner = string_tools::pod_to_hex(cryptonote::get_service_node_winner_from_tx_extra(blk.miner_tx.extra));
  }

  /// All the common (untemplated) code for use_bootstrap_daemon_if_necessary.  Returns a held lock
  /// if we need to bootstrap, an unheld one if we don't.
  std::unique_lock<std::shared_mutex> core_rpc_server::should_bootstrap_lock()
  {
    // TODO - support bootstrapping via a remote LMQ RPC; requires some argument fiddling

    if (!m_should_use_bootstrap_daemon)
        return {};

    std::unique_lock lock{m_bootstrap_daemon_mutex};
    if (!m_bootstrap_daemon)
    {
      lock.unlock();
      return lock;
    }

    auto current_time = std::chrono::system_clock::now();
    if (!m_p2p.get_payload_object().no_sync() &&
        current_time - m_bootstrap_height_check_time > 30s)  // update every 30s
    {
      m_bootstrap_height_check_time = current_time;

      std::optional<uint64_t> bootstrap_daemon_height = m_bootstrap_daemon->get_height();
      if (!bootstrap_daemon_height)
      {
        MERROR("Failed to fetch bootstrap daemon height");
        lock.unlock();
        return lock;
      }

      uint64_t target_height = m_core.get_target_blockchain_height();
      if (bootstrap_daemon_height < target_height)
      {
        MINFO("Bootstrap daemon is out of sync");
        lock.unlock();
        m_bootstrap_daemon->handle_result(false);
        return lock;
      }

      uint64_t top_height           = m_core.get_current_blockchain_height();
      m_should_use_bootstrap_daemon = top_height + 10 < bootstrap_daemon_height;
      MINFO((m_should_use_bootstrap_daemon ? "Using" : "Not using") << " the bootstrap daemon (our height: " << top_height << ", bootstrap daemon's height: " << *bootstrap_daemon_height << ")");
    }

    if (!m_should_use_bootstrap_daemon)
    {
      MINFO("The local daemon is fully synced; disabling bootstrap daemon requests");
      lock.unlock();
    }

    return lock;
  }

  //------------------------------------------------------------------------------------------------------------------------------
  // If we have a bootstrap daemon configured and we haven't fully synched yet then forward the
  // request to the bootstrap daemon.  Returns true if the request was bootstrapped, false if the
  // request shouldn't be bootstrapped, and throws an exception if the bootstrap request fails.
  //
  // The RPC type must have a `bool untrusted` member.
  //
  template <typename RPC>
  bool core_rpc_server::use_bootstrap_daemon_if_necessary(const typename RPC::request& req, typename RPC::response& res)
  {
    res.untrusted = false; // If compilation fails here then the type being instantiated doesn't support using a bootstrap daemon
    auto bs_lock = should_bootstrap_lock();
    if (!bs_lock)
      return false;

    std::string command_name{RPC::names().front()};

    bool success;
    if (std::is_base_of<BINARY, RPC>::value)
      success = m_bootstrap_daemon->invoke_http_bin(command_name, req, res);
    else
    {
      // FIXME: this type explosion of having to instantiate nested types is an epee pain point:
      // epee is only incapable of nested serialization if you build nested C++ classes mimicing the
      // JSON nesting.  Ew.
      epee::json_rpc::request<typename RPC::request> json_req{};
      epee::json_rpc::response_with_error<typename RPC::response> json_resp{};
      json_req.jsonrpc = "2.0";
      json_req.id = epee::serialization::storage_entry(0);
      json_req.method = command_name;
      json_req.params = req;
      success = m_bootstrap_daemon->invoke_http_json_rpc(command_name, json_req, json_resp);
      if (success)
        res = std::move(json_resp.result);
    }

    if (!success)
      throw std::runtime_error{"Bootstrap request failed"};

    m_was_bootstrap_ever_used = true;
    res.untrusted = true;
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_LAST_BLOCK_HEADER::response core_rpc_server::invoke(GET_LAST_BLOCK_HEADER::request&& req, rpc_context context)
  {
    GET_LAST_BLOCK_HEADER::response res{};

    PERF_TIMER(on_get_last_block_header);
    if (use_bootstrap_daemon_if_necessary<GET_LAST_BLOCK_HEADER>(req, res))
      return res;

    CHECK_CORE_READY();
    uint64_t last_block_height;
    crypto::hash last_block_hash;
    m_core.get_blockchain_top(last_block_height, last_block_hash);
    block last_block;
    bool have_last_block = m_core.get_block_by_hash(last_block_hash, last_block);
    if (!have_last_block)
      throw rpc_error{ERROR_INTERNAL, "Internal error: can't get last block."};
    fill_block_header_response(last_block, false, last_block_height, last_block_hash, res.block_header, req.fill_pow_hash && context.admin);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_BLOCK_HEADER_BY_HASH::response core_rpc_server::invoke(GET_BLOCK_HEADER_BY_HASH::request&& req, rpc_context context)
  {
    GET_BLOCK_HEADER_BY_HASH::response res{};

    PERF_TIMER(on_get_block_header_by_hash);
    if (use_bootstrap_daemon_if_necessary<GET_BLOCK_HEADER_BY_HASH>(req, res))
      return res;

    auto get = [this](const std::string &hash, bool fill_pow_hash, block_header_response &block_header, bool admin) -> void {
      crypto::hash block_hash;
      bool hash_parsed = parse_hash256(hash, block_hash);
      if(!hash_parsed)
        throw rpc_error{ERROR_WRONG_PARAM, "Failed to parse hex representation of block hash. Hex = " + hash + '.'};
      block blk;
      bool orphan = false;
      bool have_block = m_core.get_block_by_hash(block_hash, blk, &orphan);
      if (!have_block)
        throw rpc_error{ERROR_INTERNAL, "Internal error: can't get block by hash. Hash = " + hash + '.'};
      if (blk.miner_tx.vin.size() != 1 || !std::holds_alternative<txin_gen>(blk.miner_tx.vin.front()))
        throw rpc_error{ERROR_INTERNAL, "Internal error: coinbase transaction in the block has the wrong type"};
      uint64_t block_height = std::get<txin_gen>(blk.miner_tx.vin.front()).height;
      fill_block_header_response(blk, orphan, block_height, block_hash, block_header, fill_pow_hash && admin);
    };

    if (!req.hash.empty())
      get(req.hash, req.fill_pow_hash, res.block_header, context.admin);

    res.block_headers.reserve(req.hashes.size());
    for (const std::string &hash: req.hashes)
    {
      res.block_headers.push_back({});
      get(hash, req.fill_pow_hash, res.block_headers.back(), context.admin);
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_BLOCK_HEADERS_RANGE::response core_rpc_server::invoke(GET_BLOCK_HEADERS_RANGE::request&& req, rpc_context context)
  {
    GET_BLOCK_HEADERS_RANGE::response res{};

    PERF_TIMER(on_get_block_headers_range);
    if (use_bootstrap_daemon_if_necessary<GET_BLOCK_HEADERS_RANGE>(req, res))
      return res;

    const uint64_t bc_height = m_core.get_current_blockchain_height();
    if (req.start_height >= bc_height || req.end_height >= bc_height || req.start_height > req.end_height)
      throw rpc_error{ERROR_TOO_BIG_HEIGHT, "Invalid start/end heights."};
    for (uint64_t h = req.start_height; h <= req.end_height; ++h)
    {
      crypto::hash block_hash = m_core.get_block_id_by_height(h);
      block blk;
      bool have_block = m_core.get_block_by_hash(block_hash, blk);
      if (!have_block)
        throw rpc_error{ERROR_INTERNAL, 
          "Internal error: can't get block by height. Height = " + std::to_string(h) + ". Hash = " + epee::string_tools::pod_to_hex(block_hash) + '.'};
      if (blk.miner_tx.vin.size() != 1 || !std::holds_alternative<txin_gen>(blk.miner_tx.vin.front()))
        throw rpc_error{ERROR_INTERNAL, "Internal error: coinbase transaction in the block has the wrong type"};
      uint64_t block_height = std::get<txin_gen>(blk.miner_tx.vin.front()).height;
      if (block_height != h)
        throw rpc_error{ERROR_INTERNAL, "Internal error: coinbase transaction in the block has the wrong height"};
      res.headers.push_back(block_header_response());
      fill_block_header_response(blk, false, block_height, block_hash, res.headers.back(), req.fill_pow_hash && context.admin);
    }
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_BLOCK_HEADER_BY_HEIGHT::response core_rpc_server::invoke(GET_BLOCK_HEADER_BY_HEIGHT::request&& req, rpc_context context)
  {
    GET_BLOCK_HEADER_BY_HEIGHT::response res{};

    PERF_TIMER(on_get_block_header_by_height);
    if (use_bootstrap_daemon_if_necessary<GET_BLOCK_HEADER_BY_HEIGHT>(req, res))
      return res;

    if(m_core.get_current_blockchain_height() <= req.height)
      throw rpc_error{ERROR_TOO_BIG_HEIGHT,
        "Requested block height: " + std::to_string(req.height) + " greater than current top block height: " +  std::to_string(m_core.get_current_blockchain_height() - 1)};
    crypto::hash block_hash = m_core.get_block_id_by_height(req.height);
    block blk;
    bool have_block = m_core.get_block_by_hash(block_hash, blk);
    if (!have_block)
      throw rpc_error{ERROR_INTERNAL, "Internal error: can't get block by height. Height = " + std::to_string(req.height) + '.'};
    fill_block_header_response(blk, false, req.height, block_hash, res.block_header, req.fill_pow_hash && context.admin);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_BLOCK::response core_rpc_server::invoke(GET_BLOCK::request&& req, rpc_context context)
  {
    GET_BLOCK::response res{};

    PERF_TIMER(on_get_block);
    if (use_bootstrap_daemon_if_necessary<GET_BLOCK>(req, res))
      return res;

    crypto::hash block_hash;
    if (!req.hash.empty())
    {
      bool hash_parsed = parse_hash256(req.hash, block_hash);
      if(!hash_parsed)
        throw rpc_error{ERROR_WRONG_PARAM, "Failed to parse hex representation of block hash. Hex = " + req.hash + '.'};
    }
    else
    {
      if(m_core.get_current_blockchain_height() <= req.height)
        throw rpc_error{ERROR_TOO_BIG_HEIGHT, std::string("Requested block height: ") + std::to_string(req.height) + " greater than current top block height: " +  std::to_string(m_core.get_current_blockchain_height() - 1)};
      block_hash = m_core.get_block_id_by_height(req.height);
    }
    block blk;
    bool orphan = false;
    bool have_block = m_core.get_block_by_hash(block_hash, blk, &orphan);
    if (!have_block)
      throw rpc_error{ERROR_INTERNAL, "Internal error: can't get block by hash. Hash = " + req.hash + '.'};
    if (blk.miner_tx.vin.size() != 1 || !std::holds_alternative<txin_gen>(blk.miner_tx.vin.front()))
      throw rpc_error{ERROR_INTERNAL, "Internal error: coinbase transaction in the block has the wrong type"};
    uint64_t block_height = std::get<txin_gen>(blk.miner_tx.vin.front()).height;
    fill_block_header_response(blk, orphan, block_height, block_hash, res.block_header, req.fill_pow_hash && context.admin);
    for (size_t n = 0; n < blk.tx_hashes.size(); ++n)
    {
      res.tx_hashes.push_back(epee::string_tools::pod_to_hex(blk.tx_hashes[n]));
    }
    res.blob = string_tools::buff_to_hex_nodelimer(t_serializable_object_to_blob(blk));
    res.json = obj_to_json_str(blk);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_CONNECTIONS::response core_rpc_server::invoke(GET_CONNECTIONS::request&& req, rpc_context context)
  {
    GET_CONNECTIONS::response res{};

    PERF_TIMER(on_get_connections);

    res.connections = m_p2p.get_payload_object().get_connections();

    res.status = STATUS_OK;

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  HARD_FORK_INFO::response core_rpc_server::invoke(HARD_FORK_INFO::request&& req, rpc_context context)
  {
    HARD_FORK_INFO::response res{};

    PERF_TIMER(on_hard_fork_info);
    if (use_bootstrap_daemon_if_necessary<HARD_FORK_INFO>(req, res))
      return res;

    const Blockchain &blockchain = m_core.get_blockchain_storage();
    uint8_t version = req.version > 0 ? req.version : blockchain.get_next_hard_fork_version();
    res.version = blockchain.get_current_hard_fork_version();
    res.enabled = blockchain.get_hard_fork_voting_info(version, res.window, res.votes, res.threshold, res.earliest_height, res.voting);
    res.state = blockchain.get_hard_fork_state();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GETBANS::response core_rpc_server::invoke(GETBANS::request&& req, rpc_context context)
  {
    GETBANS::response res{};

    PERF_TIMER(on_get_bans);

    auto now = time(nullptr);
    std::map<std::string, time_t> blocked_hosts = m_p2p.get_blocked_hosts();
    for (std::map<std::string, time_t>::const_iterator i = blocked_hosts.begin(); i != blocked_hosts.end(); ++i)
    {
      if (i->second > now) {
        GETBANS::ban b;
        b.host = i->first;
        b.ip = 0;
        uint32_t ip;
        if (epee::string_tools::get_ip_int32_from_string(ip, b.host))
          b.ip = ip;
        b.seconds = i->second - now;
        res.bans.push_back(b);
      }
    }
    std::map<epee::net_utils::ipv4_network_subnet, time_t> blocked_subnets = m_p2p.get_blocked_subnets();
    for (std::map<epee::net_utils::ipv4_network_subnet, time_t>::const_iterator i = blocked_subnets.begin(); i != blocked_subnets.end(); ++i)
    {
      if (i->second > now) {
        GETBANS::ban b;
        b.host = i->first.host_str();
        b.ip = 0;
        b.seconds = i->second - now;
        res.bans.push_back(b);
      }
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  BANNED::response core_rpc_server::invoke(BANNED::request&& req, rpc_context context)
  {
    BANNED::response res{};

    PERF_TIMER(on_banned);

    auto na_parsed = net::get_network_address(req.address, 0);
    if (!na_parsed)
      throw rpc_error{ERROR_WRONG_PARAM, "Unsupported host type"};
    epee::net_utils::network_address na = std::move(*na_parsed);

    time_t seconds;
    if (m_p2p.is_host_blocked(na, &seconds))
    {
      res.banned = true;
      res.seconds = seconds;
    }
    else
    {
      res.banned = false;
      res.seconds = 0;
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SETBANS::response core_rpc_server::invoke(SETBANS::request&& req, rpc_context context)
  {
    SETBANS::response res{};

    PERF_TIMER(on_set_bans);

    for (auto i = req.bans.begin(); i != req.bans.end(); ++i)
    {
      epee::net_utils::network_address na;

      // try subnet first
      if (!i->host.empty())
      {
        auto ns_parsed = net::get_ipv4_subnet_address(i->host);
        if (ns_parsed)
        {
          if (i->ban)
            m_p2p.block_subnet(*ns_parsed, i->seconds);
          else
            m_p2p.unblock_subnet(*ns_parsed);
          continue;
        }
      }

      // then host
      if (!i->host.empty())
      {
        auto na_parsed = net::get_network_address(i->host, 0);
        if (!na_parsed)
          throw rpc_error{ERROR_WRONG_PARAM, "Unsupported host/subnet type"};
        na = std::move(*na_parsed);
      }
      else
      {
        na = epee::net_utils::ipv4_network_address{i->ip, 0};
      }
      if (i->ban)
        m_p2p.block_host(na, i->seconds);
      else
        m_p2p.unblock_host(na);
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  FLUSH_TRANSACTION_POOL::response core_rpc_server::invoke(FLUSH_TRANSACTION_POOL::request&& req, rpc_context context)
  {
    FLUSH_TRANSACTION_POOL::response res{};

    PERF_TIMER(on_flush_txpool);

    bool failed = false;
    std::vector<crypto::hash> txids;
    if (req.txids.empty())
    {
      std::vector<transaction> pool_txs;
      m_core.get_pool().get_transactions(pool_txs);
      for (const auto &tx: pool_txs)
      {
        txids.push_back(cryptonote::get_transaction_hash(tx));
      }
    }
    else
    {
      for (const auto &str: req.txids)
      {
        cryptonote::blobdata txid_data;
        if(!epee::string_tools::parse_hexstr_to_binbuff(str, txid_data))
        {
          failed = true;
        }
        else
        {
          crypto::hash txid = *reinterpret_cast<const crypto::hash*>(txid_data.data());
          txids.push_back(txid);
        }
      }
    }
    if (!m_core.get_blockchain_storage().flush_txes_from_pool(txids))
    {
      res.status = "Failed to remove one or more tx(es)";
      return res;
    }

    res.status = failed
      ? txids.empty()
        ? "Failed to parse txid"
        : "Failed to parse some of the txids"
      : STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_OUTPUT_HISTOGRAM::response core_rpc_server::invoke(GET_OUTPUT_HISTOGRAM::request&& req, rpc_context context)
  {
    GET_OUTPUT_HISTOGRAM::response res{};

    PERF_TIMER(on_get_output_histogram);
    if (use_bootstrap_daemon_if_necessary<GET_OUTPUT_HISTOGRAM>(req, res))
      return res;

    if (!context.admin && req.recent_cutoff > 0 && req.recent_cutoff < (uint64_t)time(NULL) - OUTPUT_HISTOGRAM_RECENT_CUTOFF_RESTRICTION)
    {
      res.status = "Recent cutoff is too old";
      return res;
    }

    std::map<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>> histogram;
    try
    {
      histogram = m_core.get_blockchain_storage().get_output_histogram(req.amounts, req.unlocked, req.recent_cutoff, req.min_count);
    }
    catch (const std::exception &e)
    {
      res.status = "Failed to get output histogram";
      return res;
    }

    res.histogram.clear();
    res.histogram.reserve(histogram.size());
    for (const auto &i: histogram)
    {
      if (std::get<0>(i.second) >= req.min_count && (std::get<0>(i.second) <= req.max_count || req.max_count == 0))
        res.histogram.push_back(GET_OUTPUT_HISTOGRAM::entry(i.first, std::get<0>(i.second), std::get<1>(i.second), std::get<2>(i.second)));
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_VERSION::response core_rpc_server::invoke(GET_VERSION::request&& req, rpc_context context)
  {
    GET_VERSION::response res{};

    PERF_TIMER(on_get_version);
    if (use_bootstrap_daemon_if_necessary<GET_VERSION>(req, res))
      return res;

    res.version = pack_version(VERSION);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_SERVICE_NODE_STATUS::response core_rpc_server::invoke(GET_SERVICE_NODE_STATUS::request&& req, rpc_context context)
  {
    GET_SERVICE_NODE_STATUS::response res{};

    PERF_TIMER(on_get_service_node_status);
    auto get_service_node_key_res = invoke(GET_SERVICE_KEYS::request{}, context);

    GET_SERVICE_NODES::request get_service_nodes_req{};
    get_service_nodes_req.include_json = req.include_json;
    get_service_nodes_req.service_node_pubkeys.push_back(std::move(get_service_node_key_res.service_node_pubkey));

    auto get_service_nodes_res = invoke(std::move(get_service_nodes_req), context);
    res.status = get_service_nodes_res.status;

    if (get_service_nodes_res.service_node_states.empty()) // Started in service node but not staked, no information on the blockchain yet
    {
      res.service_node_state.service_node_pubkey  = std::move(get_service_node_key_res.service_node_pubkey);
      res.service_node_state.version_major        = SISPOP_VERSION[0];
      res.service_node_state.version_minor        = SISPOP_VERSION[1];
      res.service_node_state.version_patch        = SISPOP_VERSION[2];
      res.service_node_state.public_ip            = epee::string_tools::get_ip_string_from_int32(m_core.sn_public_ip());
      res.service_node_state.storage_port         = m_core.storage_port();
      res.service_node_state.storage_lmq_port     = m_core.m_storage_lmq_port;
      res.service_node_state.quorumnet_port       = m_core.quorumnet_port();
      res.service_node_state.pubkey_ed25519       = std::move(get_service_node_key_res.service_node_ed25519_pubkey);
      res.service_node_state.pubkey_x25519        = std::move(get_service_node_key_res.service_node_x25519_pubkey);
      res.service_node_state.service_node_version = SISPOP_VERSION;
    }
    else
    {
      res.service_node_state = std::move(get_service_nodes_res.service_node_states[0]);
    }

    res.height = get_service_nodes_res.height;
    res.block_hash = get_service_nodes_res.block_hash;
    res.status = get_service_nodes_res.status;
    res.as_json = get_service_nodes_res.as_json;

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_COINBASE_TX_SUM::response core_rpc_server::invoke(GET_COINBASE_TX_SUM::request&& req, rpc_context context)
  {
    GET_COINBASE_TX_SUM::response res{};

    PERF_TIMER(on_get_coinbase_tx_sum);
    std::tie(res.emission_amount, res.fee_amount, res.burn_amount) = m_core.get_coinbase_tx_sum(req.height, req.count);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_BASE_FEE_ESTIMATE::response core_rpc_server::invoke(GET_BASE_FEE_ESTIMATE::request&& req, rpc_context context)
  {
    GET_BASE_FEE_ESTIMATE::response res{};

    PERF_TIMER(on_get_base_fee_estimate);
    if (use_bootstrap_daemon_if_necessary<GET_BASE_FEE_ESTIMATE>(req, res))
      return res;

    auto fees = m_core.get_blockchain_storage().get_dynamic_base_fee_estimate(req.grace_blocks);
    res.fee_per_byte = fees.first;
    res.fee_per_output = fees.second;
    res.quantization_mask = Blockchain::get_fee_quantization_mask();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_ALTERNATE_CHAINS::response core_rpc_server::invoke(GET_ALTERNATE_CHAINS::request&& req, rpc_context context)
  {
    GET_ALTERNATE_CHAINS::response res{};

    PERF_TIMER(on_get_alternate_chains);
    try
    {
      std::vector<std::pair<Blockchain::block_extended_info, std::vector<crypto::hash>>> chains = m_core.get_blockchain_storage().get_alternative_chains();
      for (const auto &i: chains)
      {
        res.chains.push_back(GET_ALTERNATE_CHAINS::chain_info{epee::string_tools::pod_to_hex(get_block_hash(i.first.bl)), i.first.height, i.second.size(), i.first.cumulative_difficulty, {}, std::string()});
        res.chains.back().block_hashes.reserve(i.second.size());
        for (const crypto::hash &block_id: i.second)
          res.chains.back().block_hashes.push_back(epee::string_tools::pod_to_hex(block_id));
        if (i.first.height < i.second.size())
        {
          res.status = "Error finding alternate chain attachment point";
          return res;
        }
        cryptonote::block main_chain_parent_block;
        try { main_chain_parent_block = m_core.get_blockchain_storage().get_db().get_block_from_height(i.first.height - i.second.size()); }
        catch (const std::exception &e) { res.status = "Error finding alternate chain attachment point"; return res; }
        res.chains.back().main_chain_parent_block = epee::string_tools::pod_to_hex(get_block_hash(main_chain_parent_block));
      }
      res.status = STATUS_OK;
    }
    catch (...)
    {
      res.status = "Error retrieving alternate chains";
    }
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_LIMIT::response core_rpc_server::invoke(GET_LIMIT::request&& req, rpc_context context)
  {
    GET_LIMIT::response res{};

    PERF_TIMER(on_get_limit);
    if (use_bootstrap_daemon_if_necessary<GET_LIMIT>(req, res))
      return res;

    res.limit_down = epee::net_utils::connection_basic::get_rate_down_limit();
    res.limit_up = epee::net_utils::connection_basic::get_rate_up_limit();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SET_LIMIT::response core_rpc_server::invoke(SET_LIMIT::request&& req, rpc_context context)
  {
    SET_LIMIT::response res{};

    PERF_TIMER(on_set_limit);
    // -1 = reset to default
    //  0 = do not modify

    if (req.limit_down < -1 || req.limit_up < -1)
      throw rpc_error{ERROR_WRONG_PARAM, "Invalid limit_down or limit_up value: value must be >= -1"};

    if (req.limit_down != 0)
      epee::net_utils::connection_basic::set_rate_down_limit(
          req.limit_down == -1 ? nodetool::default_limit_down : req.limit_down);
    if (req.limit_up != 0)
      epee::net_utils::connection_basic::set_rate_up_limit(
          req.limit_up == -1 ? nodetool::default_limit_up : req.limit_up);

    res.limit_down = epee::net_utils::connection_basic::get_rate_down_limit();
    res.limit_up = epee::net_utils::connection_basic::get_rate_up_limit();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  OUT_PEERS::response core_rpc_server::invoke(OUT_PEERS::request&& req, rpc_context context)
  {
    OUT_PEERS::response res{};

    PERF_TIMER(on_out_peers);
    if (req.set)
      m_p2p.change_max_out_public_peers(req.out_peers);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  IN_PEERS::response core_rpc_server::invoke(IN_PEERS::request&& req, rpc_context context)
  {
    IN_PEERS::response res{};

    PERF_TIMER(on_in_peers);
    if (req.set)
      m_p2p.change_max_in_public_peers(req.in_peers);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  UPDATE::response core_rpc_server::invoke(UPDATE::request&& req, rpc_context context)
  {
    UPDATE::response res{};

    PERF_TIMER(on_update);

    if (m_core.offline())
    {
      res.status = "Daemon is running offline";
      return res;
    }

    static const char software[] = "sispop";
#ifdef BUILD_TAG
    static const char buildtag[] = BOOST_PP_STRINGIZE(BUILD_TAG);
    static const char subdir[] = "cli";
#else
    static const char buildtag[] = "source";
    static const char subdir[] = "source";
#endif

    if (req.command != "check" && req.command != "download" && req.command != "update")
    {
      res.status = "unknown command: '" + req.command + "'";
      return res;
    }

    std::string version, hash;
    if (!tools::check_updates(software, buildtag, version, hash))
    {
      res.status = "Error checking for updates";
      return res;
    }
    if (tools::vercmp(version.c_str(), SISPOP_VERSION_STR) <= 0)
    {
      res.update = false;
      res.status = STATUS_OK;
      return res;
    }
    res.update = true;
    res.version = version;
    res.user_uri = tools::get_update_url(software, subdir, buildtag, version, true);
    res.auto_uri = tools::get_update_url(software, subdir, buildtag, version, false);
    res.hash = hash;
    if (req.command == "check")
    {
      res.status = STATUS_OK;
      return res;
    }

    boost::filesystem::path path;
    if (req.path.empty())
    {
      std::string filename;
      const char *slash = strrchr(res.auto_uri.c_str(), '/');
      if (slash)
        filename = slash + 1;
      else
        filename = std::string(software) + "-update-" + version;
      path = epee::string_tools::get_current_module_folder();
      path /= filename;
    }
    else
    {
      path = req.path;
    }

    crypto::hash file_hash;
    if (!tools::sha256sum_file(path.string(), file_hash) || (hash != epee::string_tools::pod_to_hex(file_hash)))
    {
      MDEBUG("We don't have that file already, downloading");
      if (!tools::download(path.string(), res.auto_uri))
      {
        MERROR("Failed to download " << res.auto_uri);
        res.status = "Failed to download";
        return res;
      }
      if (!tools::sha256sum_file(path.string(), file_hash))
      {
        MERROR("Failed to hash " << path);
        res.status = "Failed to hash";
        return res;
      }
      if (hash != epee::string_tools::pod_to_hex(file_hash))
      {
        MERROR("Download from " << res.auto_uri << " does not match the expected hash");
        res.status = "Failed: hash mismatch";
        return res;
      }
      MINFO("New version downloaded to " << path);
    }
    else
    {
      MDEBUG("We already have " << path << " with expected hash");
    }
    res.path = path.string();

    if (req.command == "download")
    {
      res.status = STATUS_OK;
      return res;
    }

    res.status = "'update' not implemented yet";
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  POP_BLOCKS::response core_rpc_server::invoke(POP_BLOCKS::request&& req, rpc_context context)
  {
    POP_BLOCKS::response res{};

    PERF_TIMER(on_pop_blocks);

    m_core.get_blockchain_storage().pop_blocks(req.nblocks);

    res.height = m_core.get_current_blockchain_height();
    res.status = STATUS_OK;

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  RELAY_TX::response core_rpc_server::invoke(RELAY_TX::request&& req, rpc_context context)
  {
    RELAY_TX::response res{};

    PERF_TIMER(on_relay_tx);

    res.status = "";
    for (const auto &str: req.txids)
    {
      cryptonote::blobdata txid_data;
      if(!epee::string_tools::parse_hexstr_to_binbuff(str, txid_data))
      {
        if (!res.status.empty()) res.status += ", ";
        res.status += "invalid transaction id: " + str;
        continue;
      }
      crypto::hash txid = *reinterpret_cast<const crypto::hash*>(txid_data.data());

      cryptonote::blobdata txblob;
      bool r = m_core.get_pool().get_transaction(txid, txblob);
      if (r)
      {
        cryptonote_connection_context fake_context{};
        NOTIFY_NEW_TRANSACTIONS::request r{};
        r.txs.push_back(txblob);
        m_core.get_protocol()->relay_transactions(r, fake_context);
        //TODO: make sure that tx has reached other nodes here, probably wait to receive reflections from other nodes
      }
      else
      {
        if (!res.status.empty()) res.status += ", ";
        res.status += "transaction not found in pool: " + str;
        continue;
      }
    }

    if (res.status.empty())
      res.status = STATUS_OK;

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SYNC_INFO::response core_rpc_server::invoke(SYNC_INFO::request&& req, rpc_context context)
  {
    SYNC_INFO::response res{};

    PERF_TIMER(on_sync_info);

    crypto::hash top_hash;
    m_core.get_blockchain_top(res.height, top_hash);
    ++res.height; // turn top block height into blockchain height
    res.target_height = m_core.get_target_blockchain_height();
    res.next_needed_pruning_seed = m_p2p.get_payload_object().get_next_needed_pruning_stripe().second;

    for (const auto &c: m_p2p.get_payload_object().get_connections())
      res.peers.push_back({c});
    const cryptonote::block_queue &block_queue = m_p2p.get_payload_object().get_block_queue();
    block_queue.foreach([&](const cryptonote::block_queue::span &span) {
      const std::string span_connection_id = epee::string_tools::pod_to_hex(span.connection_id);
      uint32_t speed = (uint32_t)(100.0f * block_queue.get_speed(span.connection_id) + 0.5f);
      std::string address = "";
      for (const auto &c: m_p2p.get_payload_object().get_connections())
        if (c.connection_id == span_connection_id)
          address = c.address;
      res.spans.push_back({span.start_block_height, span.nblocks, span_connection_id, (uint32_t)(span.rate + 0.5f), speed, span.size, address});
      return true;
    });
    res.overview = block_queue.get_overview(res.height);

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_TRANSACTION_POOL_BACKLOG::response core_rpc_server::invoke(GET_TRANSACTION_POOL_BACKLOG::request&& req, rpc_context context)
  {
    GET_TRANSACTION_POOL_BACKLOG::response res{};

    PERF_TIMER(on_get_txpool_backlog);
    if (use_bootstrap_daemon_if_necessary<GET_TRANSACTION_POOL_BACKLOG>(req, res))
      return res;

    m_core.get_pool().get_transaction_backlog(res.backlog);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_OUTPUT_DISTRIBUTION::response core_rpc_server::invoke(GET_OUTPUT_DISTRIBUTION::request&& req, rpc_context context)
  {
    GET_OUTPUT_DISTRIBUTION::response res{};

    PERF_TIMER(on_get_output_distribution);
    if (use_bootstrap_daemon_if_necessary<GET_OUTPUT_DISTRIBUTION>(req, res))
      return res;

    try
    {
      // 0 is placeholder for the whole chain
      const uint64_t req_to_height = req.to_height ? req.to_height : (m_core.get_current_blockchain_height() - 1);
      for (uint64_t amount: req.amounts)
      {
        auto data = RpcHandler::get_output_distribution(
            [this](auto&&... args) { return m_core.get_output_distribution(std::forward<decltype(args)>(args)...); },
            amount,
            req.from_height,
            req_to_height,
            [this](uint64_t height) { return m_core.get_blockchain_storage().get_db().get_block_hash_from_height(height); },
            req.cumulative,
            m_core.get_current_blockchain_height());
        if (!data)
          throw rpc_error{ERROR_INTERNAL, "Failed to get output distribution"};

        res.distributions.push_back({std::move(*data), amount, "", req.binary, req.compress});
      }
    }
    catch (const std::exception &e)
    {
      throw rpc_error{ERROR_INTERNAL, "Failed to get output distribution"};
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_OUTPUT_DISTRIBUTION_BIN::response core_rpc_server::invoke(GET_OUTPUT_DISTRIBUTION_BIN::request&& req, rpc_context context)
  {
    GET_OUTPUT_DISTRIBUTION_BIN::response res{};

    PERF_TIMER(on_get_output_distribution_bin);

    if (!req.binary)
    {
      res.status = "Binary only call";
      return res;
    }

    if (use_bootstrap_daemon_if_necessary<GET_OUTPUT_DISTRIBUTION_BIN>(req, res))
      return res;

    return invoke(std::move(static_cast<GET_OUTPUT_DISTRIBUTION::request&>(req)), context);
  }
  //------------------------------------------------------------------------------------------------------------------------------
  PRUNE_BLOCKCHAIN::response core_rpc_server::invoke(PRUNE_BLOCKCHAIN::request&& req, rpc_context context)
  {
    PRUNE_BLOCKCHAIN::response res{};

    try
    {
      if (!(req.check ? m_core.check_blockchain_pruning() : m_core.prune_blockchain()))
        throw rpc_error{ERROR_INTERNAL, req.check ? "Failed to check blockchain pruning" : "Failed to prune blockchain"};
      res.pruning_seed = m_core.get_blockchain_pruning_seed();
      res.pruned = res.pruning_seed != 0;
    }
    catch (const std::exception &e)
    {
      throw rpc_error{ERROR_INTERNAL, "Failed to prune blockchain"};
    }

    res.status = STATUS_OK;
    return res;
  }

  GET_QUORUM_STATE::response core_rpc_server::invoke(GET_QUORUM_STATE::request&& req, rpc_context context)
  {
    GET_QUORUM_STATE::response res{};

    PERF_TIMER(on_get_quorum_state);

    if (req.quorum_type >= tools::enum_count<service_nodes::quorum_type> &&
        req.quorum_type != GET_QUORUM_STATE::ALL_QUORUMS_SENTINEL_VALUE)
      throw rpc_error{ERROR_WRONG_PARAM,
        "Quorum type specifies an invalid value: " + std::to_string(req.quorum_type)};

    uint64_t start = req.start_height, end = req.end_height;
    if (start == GET_QUORUM_STATE::HEIGHT_SENTINEL_VALUE &&
        end == GET_QUORUM_STATE::HEIGHT_SENTINEL_VALUE)
    {
      start = m_core.get_blockchain_storage().get_current_blockchain_height() - 1;
      end   = start + 1;
    }
    else if (start == GET_QUORUM_STATE::HEIGHT_SENTINEL_VALUE)
    {
      start = end;
      end   = end + 1;
    }
    else if (end == GET_QUORUM_STATE::HEIGHT_SENTINEL_VALUE)
    {
      end = start + 1;
    }
    else
    {
      if (end > start) end++;
      else
      {
        if (end != 0)
          end--;
      }
    }

    uint64_t curr_height = m_core.get_blockchain_storage().get_current_blockchain_height();
    start                = std::min(curr_height, start);
    end                  = std::min(curr_height, end);

    uint64_t count       = (start > end) ? start - end : end - start;
    if (!context.admin && count > GET_QUORUM_STATE::MAX_COUNT)
      throw rpc_error{ERROR_WRONG_PARAM,
        "Number of requested quorums greater than the allowed limit: "
          + std::to_string(GET_QUORUM_STATE::MAX_COUNT)
          + ", requested: " + std::to_string(count)};

    bool at_least_one_succeeded = false;
    res.quorums.reserve(std::min((uint64_t)16, count));
    for (size_t height = start; height != end;)
    {
      uint8_t hf_version = m_core.get_hard_fork_version(height);
      if (hf_version != HardFork::INVALID_HF_VERSION)
      {
        auto start_quorum_iterator = static_cast<service_nodes::quorum_type>(0);
        auto end_quorum_iterator   = service_nodes::max_quorum_type_for_hf(hf_version);

        if (req.quorum_type != GET_QUORUM_STATE::ALL_QUORUMS_SENTINEL_VALUE)
        {
          start_quorum_iterator = static_cast<service_nodes::quorum_type>(req.quorum_type);
          end_quorum_iterator   = start_quorum_iterator;
        }

        for (int quorum_int = (int)start_quorum_iterator; quorum_int <= (int)end_quorum_iterator; quorum_int++)
        {
          auto type = static_cast<service_nodes::quorum_type>(quorum_int);
          if (std::shared_ptr<const service_nodes::quorum> quorum = m_core.get_quorum(type, height, true /*include_old*/))
          {
            GET_QUORUM_STATE::quorum_for_height entry = {};
            entry.height                                          = height;
            entry.quorum_type                                     = static_cast<uint8_t>(quorum_int);

            entry.quorum.validators.reserve(quorum->validators.size());
            entry.quorum.workers.reserve(quorum->workers.size());
            auto const &service_node_list = m_core.get_service_node_list();
            uint64_t const now = time(nullptr);

            service_node_list.for_each_service_node_info_and_proof(
             quorum->validators.begin(),
             quorum->validators.end(),
             [&](auto& pub_key, auto&, auto& proof) {
               cryptonote::rpc::GET_QUORUM_STATE::quorum_validator validator;

               validator.hash = sispopmq::to_hex(tools::view_guts(pub_key));
               validator.uptime = now - proof.timestamp;

               entry.quorum.validators.push_back(validator);
             });

            service_node_list.for_each_service_node_info_and_proof(
              quorum->workers.begin(),
              quorum->workers.end(),
              [&](auto& pub_key, auto&, auto& proof) {
                cryptonote::rpc::GET_QUORUM_STATE::quorum_worker worker;

                worker.hash = sispopmq::to_hex(tools::view_guts(pub_key));
                worker.uptime = now - proof.timestamp;

                entry.quorum.workers.push_back(worker);
            });

            res.quorums.push_back(entry);
            at_least_one_succeeded = true;
          }
        }
      }

      if (end >= start) height++;
      else height--;
    }

    if (!at_least_one_succeeded)
      throw rpc_error{ERROR_WRONG_PARAM, "Failed to query any quorums at all"};

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  FLUSH_CACHE::response core_rpc_server::invoke(FLUSH_CACHE::request&& req, rpc_context context)
  {
    FLUSH_CACHE::response res{};
    if (req.bad_txs)
      m_core.flush_bad_txs_cache();
    if (req.bad_blocks)
      m_core.flush_invalid_blocks();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_SERVICE_NODE_REGISTRATION_CMD_RAW::response core_rpc_server::invoke(GET_SERVICE_NODE_REGISTRATION_CMD_RAW::request&& req, rpc_context context)
  {
    GET_SERVICE_NODE_REGISTRATION_CMD_RAW::response res{};

    PERF_TIMER(on_get_service_node_registration_cmd_raw);

    if (!m_core.service_node())
      throw rpc_error{ERROR_WRONG_PARAM, "Daemon has not been started in service node mode, please relaunch with --service-node flag."};

    uint8_t hf_version = m_core.get_hard_fork_version(m_core.get_current_blockchain_height());
    if (!service_nodes::make_registration_cmd(m_core.get_nettype(), hf_version, req.staking_requirement, req.args, m_core.get_service_keys(), res.registration_cmd, req.make_friendly))
      throw rpc_error{ERROR_INTERNAL, "Failed to make registration command"};

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_SERVICE_NODE_REGISTRATION_CMD::response core_rpc_server::invoke(GET_SERVICE_NODE_REGISTRATION_CMD::request&& req, rpc_context context)
  {
    GET_SERVICE_NODE_REGISTRATION_CMD::response res{};

    PERF_TIMER(on_get_service_node_registration_cmd);

    std::vector<std::string> args;

    uint64_t const curr_height   = m_core.get_current_blockchain_height();
    uint64_t staking_requirement = service_nodes::get_staking_requirement(m_core.get_nettype(), curr_height, m_core.get_hard_fork_version(curr_height));

    {
      uint64_t portions_cut;
      if (!service_nodes::get_portions_from_percent_str(req.operator_cut, portions_cut))
      {
        res.status = "Invalid value: " + req.operator_cut + ". Should be between [0-100]";
        MERROR(res.status);
        return res;
      }

      args.push_back(std::to_string(portions_cut));
    }

    for (const auto& [address, amount] : req.contributions)
    {
        uint64_t num_portions = service_nodes::get_portions_to_make_amount(staking_requirement, amount);
        args.push_back(address);
        args.push_back(std::to_string(num_portions));
    }

    GET_SERVICE_NODE_REGISTRATION_CMD_RAW::request req_old{};

    req_old.staking_requirement = req.staking_requirement;
    req_old.args = std::move(args);
    req_old.make_friendly = false;
    return invoke(std::move(req_old), context);
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_SERVICE_NODE_BLACKLISTED_KEY_IMAGES::response core_rpc_server::invoke(GET_SERVICE_NODE_BLACKLISTED_KEY_IMAGES::request&& req, rpc_context context)
  {
    GET_SERVICE_NODE_BLACKLISTED_KEY_IMAGES::response res{};

    PERF_TIMER(on_get_service_node_blacklisted_key_images);
    auto &blacklist = m_core.get_service_node_blacklisted_key_images();

    res.status = STATUS_OK;
    res.blacklist.reserve(blacklist.size());
    for (const service_nodes::key_image_blacklist_entry &entry : blacklist)
    {
      res.blacklist.emplace_back();
      auto &new_entry = res.blacklist.back();
      new_entry.key_image     = epee::string_tools::pod_to_hex(entry.key_image);
      new_entry.unlock_height = entry.unlock_height;
      new_entry.amount = entry.amount;
    }
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_SERVICE_KEYS::response core_rpc_server::invoke(GET_SERVICE_KEYS::request&& req, rpc_context context)
  {
    GET_SERVICE_KEYS::response res{};

    PERF_TIMER(on_get_service_node_key);

    const auto& keys = m_core.get_service_keys();
    if (keys.pub)
      res.service_node_pubkey = string_tools::pod_to_hex(keys.pub);
    res.service_node_ed25519_pubkey = string_tools::pod_to_hex(keys.pub_ed25519);
    res.service_node_x25519_pubkey = string_tools::pod_to_hex(keys.pub_x25519);
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_SERVICE_PRIVKEYS::response core_rpc_server::invoke(GET_SERVICE_PRIVKEYS::request&& req, rpc_context context)
  {
    GET_SERVICE_PRIVKEYS::response res{};

    PERF_TIMER(on_get_service_node_key);

    const auto& keys = m_core.get_service_keys();
    if (keys.key != crypto::null_skey)
      res.service_node_privkey = string_tools::pod_to_hex(keys.key.data);
    res.service_node_ed25519_privkey = string_tools::pod_to_hex(keys.key_ed25519.data);
    res.service_node_x25519_privkey = string_tools::pod_to_hex(keys.key_x25519.data);
    res.status = STATUS_OK;
    return res;
  }

  static time_t reachable_to_time_t(
      std::chrono::steady_clock::time_point t,
      std::chrono::system_clock::time_point system_now,
      std::chrono::steady_clock::time_point steady_now) {
    if (t == service_nodes::NEVER)
      return 0;
    return std::chrono::system_clock::to_time_t(system_now + (t - steady_now));
  }

  //------------------------------------------------------------------------------------------------------------------------------
  void core_rpc_server::fill_sn_response_entry(GET_SERVICE_NODES::response::entry& entry, const service_nodes::service_node_pubkey_info &sn_info, uint64_t current_height) {

    const auto &info = *sn_info.info;
    entry.service_node_pubkey           = string_tools::pod_to_hex(sn_info.pubkey);
    entry.registration_height           = info.registration_height;
    entry.requested_unlock_height       = info.requested_unlock_height;
    entry.last_reward_block_height      = info.last_reward_block_height;
    entry.last_reward_transaction_index = info.last_reward_transaction_index;
    entry.active                        = info.is_active();
    entry.funded                        = info.is_fully_funded();
    entry.state_height                  = info.is_fully_funded()
        ? (info.is_decommissioned() ? info.last_decommission_height : info.active_since_height) : info.last_reward_block_height;
    entry.earned_downtime_blocks        = service_nodes::quorum_cop::calculate_decommission_credit(info, current_height);
    entry.decommission_count            = info.decommission_count;

    auto& netconf = m_core.get_net_config();
    m_core.get_service_node_list().access_proof(sn_info.pubkey, [&entry, &netconf](const auto &proof) {
        entry.service_node_version     = proof.proof->version;
        entry.sispopnet_version          = proof.proof->sispopnet_version;
        entry.storage_server_version   = proof.proof->storage_server_version;
        entry.public_ip                = epee::string_tools::get_ip_string_from_int32(proof.proof->public_ip);
        entry.storage_port             = proof.proof->storage_https_port;
        entry.storage_lmq_port         = proof.proof->storage_omq_port;
        entry.pubkey_ed25519           = proof.proof->pubkey_ed25519 ? tools::type_to_hex(proof.proof->pubkey_ed25519) : "";
        entry.pubkey_x25519            = proof.pubkey_x25519 ? tools::type_to_hex(proof.pubkey_x25519) : "";
        entry.quorumnet_port           = proof.proof->qnet_port;

        // NOTE: Service Node Testing
        entry.last_uptime_proof                  = proof.proof->timestamp;
        auto system_now = std::chrono::system_clock::now();
        auto steady_now = std::chrono::steady_clock::now();
        entry.storage_server_reachable = !proof.ss_unreachable_for(netconf.UPTIME_PROOF_VALIDITY - netconf.UPTIME_PROOF_FREQUENCY, steady_now);
        entry.storage_server_first_unreachable = reachable_to_time_t(proof.ss_first_unreachable, system_now, steady_now);
        entry.storage_server_last_unreachable = reachable_to_time_t(proof.ss_last_unreachable, system_now, steady_now);
        entry.storage_server_last_reachable = reachable_to_time_t(proof.ss_last_reachable, system_now, steady_now);

        service_nodes::participation_history<service_nodes::participation_entry> const &checkpoint_participation = proof.checkpoint_participation;
        service_nodes::participation_history<service_nodes::participation_entry> const &pulse_participation      = proof.pulse_participation;
        service_nodes::participation_history<service_nodes::timestamp_participation_entry> const &timestamp_participation      = proof.timestamp_participation;
        service_nodes::participation_history<service_nodes::timesync_entry> const &timesync_status      = proof.timesync_status;
        entry.checkpoint_participation = std::vector<service_nodes::participation_entry>(checkpoint_participation.begin(), checkpoint_participation.end());
        entry.timestamp_participation  = std::vector<service_nodes::timestamp_participation_entry>(timestamp_participation.begin(),      timestamp_participation.end());
        entry.timesync_status          = std::vector<service_nodes::timesync_entry>(timesync_status.begin(),      timesync_status.end());
    });

    entry.contributors.reserve(info.contributors.size());

    using namespace service_nodes;
    for (service_node_info::contributor_t const &contributor : info.contributors)
    {
      entry.contributors.push_back({});
      auto &new_contributor = entry.contributors.back();
      new_contributor.amount   = contributor.amount;
      new_contributor.reserved = contributor.reserved;
      new_contributor.address  = cryptonote::get_account_address_as_str(m_core.get_nettype(), false/*is_subaddress*/, contributor.address);

      new_contributor.locked_contributions.reserve(contributor.locked_contributions.size());
      for (service_node_info::contribution_t const &src : contributor.locked_contributions)
      {
        new_contributor.locked_contributions.push_back({});
        auto &dest = new_contributor.locked_contributions.back();
        dest.amount                                                = src.amount;
        dest.key_image                                             = string_tools::pod_to_hex(src.key_image);
        dest.key_image_pub_key                                     = string_tools::pod_to_hex(src.key_image_pub_key);
      }
    }

    entry.total_contributed             = info.total_contributed;
    entry.total_reserved                = info.total_reserved;
    entry.staking_requirement           = info.staking_requirement;
    entry.portions_for_operator         = info.portions_for_operator;
    entry.operator_address              = cryptonote::get_account_address_as_str(m_core.get_nettype(), false/*is_subaddress*/, info.operator_address);
    entry.swarm_id                      = info.swarm_id;
    entry.registration_hf_version       = info.registration_hf_version;

  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_SERVICE_NODES::response core_rpc_server::invoke(GET_SERVICE_NODES::request&& req, rpc_context context)
  {
    GET_SERVICE_NODES::response res{};

    res.status = STATUS_OK;
    res.height = m_core.get_current_blockchain_height() - 1;
    res.target_height = m_core.get_target_blockchain_height();
    res.block_hash = string_tools::pod_to_hex(m_core.get_block_id_by_height(res.height));
    res.hardfork = m_core.get_hard_fork_version(res.height);

    if (!req.poll_block_hash.empty()) {
      res.polling_mode = true;
      if (req.poll_block_hash == res.block_hash) {
        res.unchanged = true;
        res.fields = req.fields;
        return res;
      }
    }

    std::vector<crypto::public_key> pubkeys(req.service_node_pubkeys.size());
    for (size_t i = 0; i < req.service_node_pubkeys.size(); i++)
    {
      if (!string_tools::hex_to_pod(req.service_node_pubkeys[i], pubkeys[i]))
        throw rpc_error{ERROR_WRONG_PARAM,
          "Could not convert to a public key, arg: " + std::to_string(i)
            + " which is pubkey: " + req.service_node_pubkeys[i]};
    }

    auto sn_infos = m_core.get_service_node_list_state(pubkeys);

    if (req.active_only) {
      const auto end =
        std::remove_if(sn_infos.begin(), sn_infos.end(), [](const service_nodes::service_node_pubkey_info& snpk_info) {
          return !snpk_info.info->is_active();
        });

      sn_infos.erase(end, sn_infos.end());
    }

    if (req.limit != 0) {

      const auto limit = std::min(sn_infos.size(), static_cast<size_t>(req.limit));

      // We need to select N random elements, in random order, from yyyyyyyy.  We could (and used
      // to) just shuffle the entire list and return the first N, but that is quite inefficient when
      // the list is large and N is small.  So instead this algorithm is going to select a random
      // element from yyyyyyyy, swap it to position 0, so we get: [x]yyyyyyyy where one of the new
      // y's used to be at element 0.  Then we select a random element from the new y's (i.e. all
      // the elements beginning at position 1), and swap it into element 1, to get [xx]yyyyyy, then
      // keep repeating until our set of x's is big enough, say [xxx]yyyyy.  At that point we chop
      // of the y's to just be left with [xxx], and only required N swaps in total.
      for (size_t i = 0; i < limit; i++)
      {
        size_t j = std::uniform_int_distribution<size_t>{i, sn_infos.size()-1}(tools::rng);
        using std::swap;
        if (i != j)
          swap(sn_infos[i], sn_infos[j]);
      }

      sn_infos.resize(limit);
    }

    res.service_node_states.reserve(sn_infos.size());
    res.fields = req.fields;

    if (req.include_json)
    {
      if (sn_infos.empty())
        res.as_json = "{}";
      else
        res.as_json = cryptonote::obj_to_json_str(sn_infos);
    }

    for (auto &pubkey_info : sn_infos) {
      res.service_node_states.emplace_back();
      fill_sn_response_entry(res.service_node_states.back(), pubkey_info, res.height);
    }

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  /// Start with seed and perform a series of computation arriving at the answer
  static uint64_t perform_blockchain_test_routine(const cryptonote::core& core,
                                                  uint64_t max_height,
                                                  uint64_t seed)
  {
    /// Should be sufficiently large to make it impractical
    /// to query remote nodes
    constexpr size_t NUM_ITERATIONS = 1000;

    std::mt19937_64 mt(seed);

    crypto::hash hash;

    uint64_t height = seed;

    for (auto i = 0u; i < NUM_ITERATIONS; ++i)
    {
      height = height % (max_height + 1);

      hash = core.get_block_id_by_height(height);

      using blob_t = cryptonote::blobdata;
      using block_pair_t = std::pair<blob_t, block>;

      /// pick a random byte from the block blob
      std::vector<block_pair_t> blocks;
      std::vector<blob_t> txs;
      if (!core.get_blockchain_storage().get_blocks(height, 1, blocks, txs)) {
        MERROR("Could not query block at requested height: " << height);
        return 0;
      }
      const blob_t &blob = blocks.at(0).first;
      const uint64_t byte_idx = tools::uniform_distribution_portable(mt, blob.size());
      uint8_t byte = blob[byte_idx];

      /// pick a random byte from a random transaction blob if found
      if (!txs.empty()) {
        const uint64_t tx_idx = tools::uniform_distribution_portable(mt, txs.size());
        const blob_t &tx_blob = txs[tx_idx];

        /// not sure if this can be empty, so check to be safe
        if (!tx_blob.empty()) {
          const uint64_t byte_idx = tools::uniform_distribution_portable(mt, tx_blob.size());
          const uint8_t tx_byte = tx_blob[byte_idx];
          byte ^= tx_byte;
        }

      }

      {
        /// reduce hash down to 8 bytes
        uint64_t n[4];
        std::memcpy(n, hash.data, sizeof(n));
        for (auto &ni : n) {
          boost::endian::little_to_native_inplace(ni);
        }

        /// Note that byte (obviously) only affects the lower byte
        /// of height, but that should be sufficient in this case
        height = n[0] ^ n[1] ^ n[2] ^ n[3] ^ byte;
      }

    }

    return height;
  }

  //------------------------------------------------------------------------------------------------------------------------------
  PERFORM_BLOCKCHAIN_TEST::response core_rpc_server::invoke(PERFORM_BLOCKCHAIN_TEST::request&& req, rpc_context context)
  {
    PERFORM_BLOCKCHAIN_TEST::response res{};

    PERF_TIMER(on_perform_blockchain_test);


    uint64_t max_height = req.max_height;
    uint64_t seed = req.seed;

    if (m_core.get_current_blockchain_height() <= max_height)
      throw rpc_error{ERROR_TOO_BIG_HEIGHT, "Requested block height too big."};

    uint64_t res_height = perform_blockchain_test_routine(m_core, max_height, seed);

    res.status = STATUS_OK;
    res.res_height = res_height;

    return res;
  }

  namespace {
    struct version_printer { const std::array<int, 3> &v; };
    std::ostream &operator<<(std::ostream &o, const version_printer &vp) { return o << vp.v[0] << '.' << vp.v[1] << '.' << vp.v[2]; }

    // Handles a ping.  Returns true if the ping was significant (i.e. first ping after startup, or
    // after the ping had expired).  `Success` is a callback that is invoked with a single boolean
    // argument: true if this ping should trigger an immediate proof send (i.e. first ping after
    // startup or after a ping expiry), false for an ordinary ping.
    template <typename RPC, typename Success>
    auto handle_ping(std::array<int, 3> cur_version, std::array<int, 3> required, const char* name, std::atomic<std::time_t>& update, time_t lifetime, Success success)
    {
      typename RPC::response res{};
      if (cur_version < required) {
        std::ostringstream status;
        status << "Outdated " << name << ". Current: " << version_printer{cur_version} << " Required: " << version_printer{required};
        res.status = status.str();
        MERROR(res.status);
      } else {
        auto now = std::time(nullptr);
        auto old = update.exchange(now);
        bool significant = old + lifetime < now; // Print loudly for the first ping after startup/expiry
        if (significant)
          MGINFO_GREEN("Received ping from " << name << " " << version_printer{cur_version});
        else
          MDEBUG("Accepted ping from " << name << " " << version_printer{cur_version});
        success(significant);
        res.status = STATUS_OK;
      }
      return res;
    }
  }

  //------------------------------------------------------------------------------------------------------------------------------
  STORAGE_SERVER_PING::response core_rpc_server::invoke(STORAGE_SERVER_PING::request&& req, rpc_context context)
  {
    return handle_ping<STORAGE_SERVER_PING>(
      {req.version_major, req.version_minor, req.version_patch}, service_nodes::MIN_STORAGE_SERVER_VERSION,
      "Storage Server", m_core.m_last_storage_server_ping, STORAGE_SERVER_PING_LIFETIME,
      [this, &req](bool significant) {
        m_core.m_storage_lmq_port = req.storage_lmq_port;
        if (significant)
          m_core.reset_proof_interval();
      });
  }
  //------------------------------------------------------------------------------------------------------------------------------
  SISPOPNET_PING::response core_rpc_server::invoke(SISPOPNET_PING::request&& req, rpc_context context)
  {
    return handle_ping<SISPOPNET_PING>(
        req.version, service_nodes::MIN_SISPOPNET_VERSION,
        "Sispopnet", m_core.m_last_sispopnet_ping, SISPOPNET_PING_LIFETIME,
        [this](bool significant) { if (significant) m_core.reset_proof_interval(); });
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_STAKING_REQUIREMENT::response core_rpc_server::invoke(GET_STAKING_REQUIREMENT::request&& req, rpc_context context)
  {
    GET_STAKING_REQUIREMENT::response res{};

    PERF_TIMER(on_get_staking_requirement);
    res.height = req.height > 0 ? req.height : m_core.get_current_blockchain_height();

    res.staking_requirement = service_nodes::get_staking_requirement(m_core.get_nettype(), res.height, m_core.get_hard_fork_version(res.height));
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  static void check_quantity_limit(size_t count, size_t max, char const *container_name = nullptr)
  {
    if (count > max)
    {
      std::ostringstream err;
      err << "Number of requested entries";
      if (container_name) err << " in " << container_name;
      err << " greater than the allowed limit: " << max << ", requested: " << count;
      throw rpc_error{ERROR_WRONG_PARAM, err.str()};
    }
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_CHECKPOINTS::response core_rpc_server::invoke(GET_CHECKPOINTS::request&& req, rpc_context context)
  {
    GET_CHECKPOINTS::response res{};

    if (use_bootstrap_daemon_if_necessary<GET_CHECKPOINTS>(req, res))
      return res;

    if (!context.admin)
      check_quantity_limit(req.count, GET_CHECKPOINTS::MAX_COUNT);

    res.status             = STATUS_OK;
    BlockchainDB const &db = m_core.get_blockchain_storage().get_db();

    std::vector<checkpoint_t> checkpoints;
    if (req.start_height == GET_CHECKPOINTS::HEIGHT_SENTINEL_VALUE &&
        req.end_height   == GET_CHECKPOINTS::HEIGHT_SENTINEL_VALUE)
    {
      checkpoint_t top_checkpoint;
      if (db.get_top_checkpoint(top_checkpoint))
        checkpoints = db.get_checkpoints_range(top_checkpoint.height, 0, req.count);
    }
    else if (req.start_height == GET_CHECKPOINTS::HEIGHT_SENTINEL_VALUE)
    {
      checkpoints = db.get_checkpoints_range(req.end_height, 0, req.count);
    }
    else if (req.end_height == GET_CHECKPOINTS::HEIGHT_SENTINEL_VALUE)
    {
      checkpoints = db.get_checkpoints_range(req.start_height, UINT64_MAX, req.count);
    }
    else
    {
      checkpoints = db.get_checkpoints_range(req.start_height, req.end_height);
    }

    res.checkpoints.reserve(checkpoints.size());
    for (checkpoint_t const &checkpoint : checkpoints)
      res.checkpoints.push_back(checkpoint);

    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  GET_SN_STATE_CHANGES::response core_rpc_server::invoke(GET_SN_STATE_CHANGES::request&& req, rpc_context context)
  {
    GET_SN_STATE_CHANGES::response res{};

    using blob_t = cryptonote::blobdata;
    using block_pair_t = std::pair<blob_t, block>;
    std::vector<block_pair_t> blocks;

    const auto& db = m_core.get_blockchain_storage();
    const uint64_t current_height = db.get_current_blockchain_height();

    uint64_t end_height;
    if (req.end_height == GET_SN_STATE_CHANGES::HEIGHT_SENTINEL_VALUE) {
      // current height is the block being mined, so exclude it from the results
      end_height = current_height - 1;
    } else {
      end_height = req.end_height;
    }

    if (end_height < req.start_height)
      throw rpc_error{ERROR_WRONG_PARAM, "The provided end_height needs to be higher than start_height"};

    if (!db.get_blocks(req.start_height, end_height - req.start_height + 1, blocks))
      throw rpc_error{ERROR_INTERNAL, "Could not query block at requested height: " + std::to_string(req.start_height)};

    res.start_height = req.start_height;
    res.end_height = end_height;

    std::vector<blob_t> blobs;
    std::vector<crypto::hash> missed_ids;
    for (const auto& block : blocks)
    {
      blobs.clear();
      if (!db.get_transactions_blobs(block.second.tx_hashes, blobs, missed_ids))
      {
        MERROR("Could not query block at requested height: " << cryptonote::get_block_height(block.second));
        continue;
      }
      const uint8_t hard_fork_version = block.second.major_version;
      for (const auto& blob : blobs)
      {
        cryptonote::transaction tx;
        if (!cryptonote::parse_and_validate_tx_from_blob(blob, tx))
        {
          MERROR("tx could not be validated from blob, possibly corrupt blockchain");
          continue;
        }
        if (tx.type == cryptonote::txtype::state_change)
        {
          cryptonote::tx_extra_service_node_state_change state_change;
          if (!cryptonote::get_service_node_state_change_from_tx_extra(tx.extra, state_change, hard_fork_version))
          {
            LOG_ERROR("Could not get state change from tx, possibly corrupt tx, hf_version "<< std::to_string(hard_fork_version));
            continue;
          }

          switch(state_change.state) {
            case service_nodes::new_state::deregister:
              res.total_deregister++;
              break;

            case service_nodes::new_state::decommission:
              res.total_decommission++;
              break;

            case service_nodes::new_state::recommission:
              res.total_recommission++;
              break;

            case service_nodes::new_state::ip_change_penalty:
              res.total_ip_change_penalty++;
              break;

            default:
              MERROR("Unhandled state in on_get_service_nodes_state_changes");
              break;
          }
        }

        if (tx.type == cryptonote::txtype::key_image_unlock)
        {
          res.total_unlock++;
        }
      }
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  REPORT_PEER_SS_STATUS::response core_rpc_server::invoke(REPORT_PEER_SS_STATUS::request&& req, rpc_context context)
  {
    REPORT_PEER_SS_STATUS::response res{};

    crypto::public_key pubkey;
    if (!string_tools::hex_to_pod(req.pubkey, pubkey)) {
      MERROR("Could not parse public key: " << req.pubkey);
      throw rpc_error{ERROR_WRONG_PARAM, "Could not parse public key"};
    }

    if (req.type != "reachability")
      throw rpc_error{ERROR_WRONG_PARAM, "Unknown status type"};
    if (!m_core.set_storage_server_peer_reachable(pubkey, req.passed))
      throw rpc_error{ERROR_WRONG_PARAM, "Pubkey not found"};

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  TEST_TRIGGER_P2P_RESYNC::response core_rpc_server::invoke(TEST_TRIGGER_P2P_RESYNC::request&& req, rpc_context context)
  {
    TEST_TRIGGER_P2P_RESYNC::response res{};

    m_p2p.reset_peer_handshake_timer();
    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  LNS_NAMES_TO_OWNERS::response core_rpc_server::invoke(LNS_NAMES_TO_OWNERS::request&& req, rpc_context context)
  {
    LNS_NAMES_TO_OWNERS::response res{};

    if (!context.admin)
      check_quantity_limit(req.entries.size(), LNS_NAMES_TO_OWNERS::MAX_REQUEST_ENTRIES);

    lns::name_system_db &db = m_core.get_blockchain_storage().name_system_db();
    for (size_t request_index = 0; request_index < req.entries.size(); request_index++)
    {
      LNS_NAMES_TO_OWNERS::request_entry const &request = req.entries[request_index];
      if (!context.admin)
        check_quantity_limit(request.types.size(), LNS_NAMES_TO_OWNERS::MAX_TYPE_REQUEST_ENTRIES, "types");

      std::vector<lns::mapping_record> records = db.get_mappings(request.types, request.name_hash);
      for (auto const &record : records)
      {
        res.entries.emplace_back();
        LNS_NAMES_TO_OWNERS::response_entry &entry = res.entries.back();
        entry.entry_index                                      = request_index;
        entry.type                                             = static_cast<uint16_t>(record.type);
        entry.name_hash                                        = record.name_hash;
        entry.owner                                            = record.owner.to_string(nettype());
        if (record.backup_owner) entry.backup_owner            = record.backup_owner.to_string(nettype());
        entry.encrypted_value                                  = sispopmq::to_hex(record.encrypted_value.to_view());
        entry.register_height                                  = record.register_height;
        entry.update_height                                    = record.update_height;
        entry.txid                                             = sispopmq::to_hex(tools::view_guts(record.txid));
        if (record.prev_txid) entry.prev_txid                  = sispopmq::to_hex(tools::view_guts(record.prev_txid));
      }
    }

    res.status = STATUS_OK;
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  LNS_OWNERS_TO_NAMES::response core_rpc_server::invoke(LNS_OWNERS_TO_NAMES::request&& req, rpc_context context)
  {
    LNS_OWNERS_TO_NAMES::response res{};

    if (!context.admin)
      check_quantity_limit(req.entries.size(), LNS_OWNERS_TO_NAMES::MAX_REQUEST_ENTRIES);

    std::unordered_map<lns::generic_owner, size_t> owner_to_request_index;
    std::vector<lns::generic_owner> owners;

    owners.reserve(req.entries.size());
    for (size_t request_index = 0; request_index < req.entries.size(); request_index++)
    {
      std::string const &owner     = req.entries[request_index];
      lns::generic_owner lns_owner = {};
      std::string errmsg;
      if (!lns::parse_owner_to_generic_owner(m_core.get_nettype(), owner, lns_owner, &errmsg))
        throw rpc_error{ERROR_WRONG_PARAM, std::move(errmsg)};

      // TODO(sispop): We now serialize both owner and backup_owner, since if
      // we specify an owner that is backup owner, we don't show the (other)
      // owner. For RPC compatibility we keep the request_index around until the
      // next hard fork (16)
      owners.push_back(lns_owner);
      owner_to_request_index[lns_owner] = request_index;
    }

    lns::name_system_db &db = m_core.get_blockchain_storage().name_system_db();
    std::vector<lns::mapping_record> records = db.get_mappings_by_owners(owners);
    for (auto &record : records)
    {
      res.entries.emplace_back();
      LNS_OWNERS_TO_NAMES::response_entry &entry = res.entries.back();

      auto it = owner_to_request_index.find(record.owner);
      if (it == owner_to_request_index.end())
        throw rpc_error{ERROR_INTERNAL, "Owner=" + record.owner.to_string(nettype()) +
          ", could not be mapped back a index in the request 'entries' array"};

      entry.request_index   = it->second;
      entry.type            = static_cast<uint16_t>(record.type);
      entry.name_hash       = std::move(record.name_hash);
      if (record.owner) entry.owner = record.owner.to_string(nettype());
      if (record.backup_owner) entry.backup_owner = record.backup_owner.to_string(nettype());
      entry.encrypted_value = sispopmq::to_hex(record.encrypted_value.to_view());
      entry.register_height = record.register_height;
      entry.update_height   = record.update_height;
      entry.txid            = sispopmq::to_hex(tools::view_guts(record.txid));
      if (record.prev_txid) entry.prev_txid = sispopmq::to_hex(tools::view_guts(record.prev_txid));
    }

    res.status = STATUS_OK;
    return res;
  }

} }  // namespace cryptonote
