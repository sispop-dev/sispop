#pragma  once

#include <functional>
#include <vector>

#include "net/http_client.h"
#include "storages/http_abstract_invoke.h"

namespace cryptonote
{

  class bootstrap_daemon
  {
  public:
    bootstrap_daemon(std::function<std::optional<std::string>()> get_next_public_node);
    bootstrap_daemon(const std::string &address, const std::optional<epee::net_utils::http::login> &credentials);

    std::string address() const noexcept;
    std::optional<uint64_t> get_height();
    bool handle_result(bool success);

    template <class t_request, class t_response>
    bool invoke_http_json(const std::string_view uri, const t_request &out_struct, t_response &result_struct)
    {
      if (!switch_server_if_needed())
      {
        return false;
      }

      return handle_result(epee::net_utils::invoke_http_json(uri, out_struct, result_struct, m_http_client));
    }

    template <class t_request, class t_response>
    bool invoke_http_bin(const std::string_view uri, const t_request &out_struct, t_response &result_struct)
    {
      if (!switch_server_if_needed())
      {
        return false;
      }

      return handle_result(epee::net_utils::invoke_http_bin(uri, out_struct, result_struct, m_http_client));
    }

    template <class t_request, class t_response>
    bool invoke_http_json_rpc(const std::string_view command_name, const t_request &out_struct, t_response &result_struct)
    {
      if (!switch_server_if_needed())
      {
        return false;
      }

      return handle_result(epee::net_utils::invoke_http_json_rpc("/json_rpc", std::string(command_name.begin(), command_name.end()), out_struct, result_struct, m_http_client));
    }

  private:
    bool set_server(const std::string &address, const std::optional<epee::net_utils::http::login> &credentials = std::nullopt);
    bool switch_server_if_needed();

  private:
    epee::net_utils::http::http_simple_client m_http_client;
    std::function<std::optional<std::string>()> m_get_next_public_node;
  };

}
