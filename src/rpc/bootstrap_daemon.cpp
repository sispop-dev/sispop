#include "bootstrap_daemon.h"

#include <stdexcept>

#include "crypto/crypto.h"
#include "cryptonote_core/cryptonote_core.h"
#include "misc_log_ex.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "daemon.rpc.bootstrap_daemon"

namespace cryptonote
{

  bootstrap_daemon::bootstrap_daemon(std::function<std::optional<std::string>()> get_next_public_node)
    : m_get_next_public_node(get_next_public_node)
  {
  }

  bootstrap_daemon::bootstrap_daemon(const std::string &address, const std::optional<epee::net_utils::http::login> &credentials)
    : bootstrap_daemon(nullptr)
  {
    if (!set_server(address, credentials))
    {
      throw std::runtime_error("invalid bootstrap daemon address or credentials");
    }
  }

  std::string bootstrap_daemon::address() const noexcept
  {
    const auto& host = m_http_client.get_host();
    if (host.empty())
    {
      return std::string();
    }
    return host + ":" + m_http_client.get_port();
  }

  std::optional<uint64_t> bootstrap_daemon::get_height()
  {
    // query bootstrap daemon's height
    cryptonote::rpc::GET_HEIGHT::request req{};
    cryptonote::rpc::GET_HEIGHT::response res{};
    if (!invoke_http_json("/getheight", req, res))
    {
      return std::nullopt;
    }

    if (res.status != cryptonote::rpc::STATUS_OK)
    {
      return std::nullopt;
    }

    return res.height;
  }

  bool bootstrap_daemon::handle_result(bool success)
  {
    if (!success && m_get_next_public_node)
    {
      m_http_client.disconnect();
    }

    return success;
  }

  bool bootstrap_daemon::set_server(const std::string &address, const std::optional<epee::net_utils::http::login> &credentials /* = std::nullopt */)
  {
    if (!m_http_client.set_server(address, credentials))
    {
      MERROR("Failed to set bootstrap daemon address " << address);
      return false;
    }

    MINFO("Changed bootstrap daemon address to " << address);
    return true;
  }


  bool bootstrap_daemon::switch_server_if_needed()
  {
    if (!m_get_next_public_node || m_http_client.is_connected())
    {
      return true;
    }

    const std::optional<std::string> address = m_get_next_public_node();
    if (address) {
      return set_server(*address);
    }

    return false;
  }

}
