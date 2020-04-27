// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#include "flatbuffer_wrapper.h"
#include "perf_client.h"

using namespace std;
using namespace nlohmann;

struct SmallBankClientOptions : public client::PerfOptions
{
  size_t total_accounts = 10;
  bool partition_clients = false;

  SmallBankClientOptions(CLI::App& app, const std::string& default_pid_file) :
    client::PerfOptions("Small_Bank_ClientCpp", default_pid_file, app)
  {
    app.add_option("--accounts", total_accounts)->capture_default_str();
    app.add_flag("--pc", partition_clients);
  }
};

using Base = client::PerfBase<SmallBankClientOptions>;

class SmallBankClient : public Base
{
private:
  enum class TransactionTypes : uint8_t
  {
    TransactSavings = 0,
    Amalgamate,
    WriteCheck,
    DepositChecking,
    GetBalance,

    NumberTransactions
  };

  const char* OPERATION_C_STR[5]{"SmallBank_transact_savings",
                                 "SmallBank_amalgamate",
                                 "SmallBank_write_check",
                                 "SmallBank_deposit_checking",
                                 "SmallBank_balance"};

  size_t from;
  size_t to;

  void print_accounts(const string& header = {})
  {
    if (!header.empty())
    {
      LOG_INFO_FMT(header);
    }

    auto conn = get_connection();

    nlohmann::json accs = nlohmann::json::array();

    for (auto i = from; i < to; i++)
    {
      BankSerializer bs(to_string(i));
      const auto response = conn->call("SmallBank_balance", bs.get_buffer());

      check_response(response);
      BalanceDeserializer bd(response.body.data());
      accs.push_back({{"account", i}, {"balance", bd.balance()}});
    }

    LOG_INFO_FMT("Accounts:\n{}", accs.dump(4));
  }

  std::optional<RpcTlsClient::Response> send_creation_transactions() override
  {

    auto connection = get_connection();
    LOG_INFO_FMT("Creating accounts from {} to {}", from, to);
    AccountsSerializer as(from, to, 1000, 1000);
    const auto response =
      connection->call("SmallBank_create_batch", as.get_buffer());
    check_response(response);

    return response;
  }

  void prepare_transactions() override
  {
    // Reserve space for transfer transactions
    prepared_txs.resize(options.num_transactions);

    for (decltype(options.num_transactions) i = 0; i < options.num_transactions;
         i++)
    {
      uint8_t operation = 0;
        //rand_range((uint8_t)TransactionTypes::NumberTransactions);

      std::unique_ptr<flatbuffers::DetachedBuffer> fb;

      switch ((TransactionTypes)operation)
      {
        case TransactionTypes::TransactSavings:
        {
          TransactionSerializer ts(
            to_string(from + rand_range(options.total_accounts)),
            rand_range<int>(-50, 50));
          fb = ts.get_detached_buffer();
        }
        break;
        case TransactionTypes::Amalgamate:
        {
          unsigned int src_account = from + rand_range(options.total_accounts);
          unsigned int dest_account = from + rand_range(options.total_accounts - 1);
          if (dest_account >= src_account)
          {
            dest_account += 1;
          }
          AmalgamateSerializer as(
            to_string(src_account), to_string(dest_account));
          fb = as.get_detached_buffer();
        }
        break;

        case TransactionTypes::WriteCheck:
        {
          TransactionSerializer ts(
            to_string(from + rand_range(options.total_accounts)), rand_range<int>(50));
          fb = ts.get_detached_buffer();
        }
        break;

        case TransactionTypes::DepositChecking:
        {
          TransactionSerializer ts(
            to_string(from + rand_range(options.total_accounts)),
            (rand_range<int>(50) + 1));
          fb = ts.get_detached_buffer();
        }
        break;

        case TransactionTypes::GetBalance:
        {
          BankSerializer bs(to_string(from + rand_range(options.total_accounts)));
          fb = bs.get_detached_buffer();
        }
        break;

        default:
          throw logic_error("Unknown operation");
      }

      add_prepared_tx(
        OPERATION_C_STR[operation],
        {fb->data(), fb->size()},
        operation != (uint8_t)TransactionTypes::GetBalance,
        i);
    }
  }

  bool check_response(const RpcTlsClient::Response& r) override
  {
    if (!http::status_success(r.status))
    {
      const std::string error_msg(r.body.begin(), r.body.end());
      if (
        error_msg.find("Not enough money in savings account") == string::npos &&
        error_msg.find("Account already exists in accounts table") ==
          string::npos)
      {
        throw logic_error(error_msg);
        return false;
      }
    }

    return true;
  }

  void pre_creation_hook() override
  {
    LOG_DEBUG_FMT("Creating {} accounts", options.total_accounts);
  }

  void post_creation_hook() override
  {
    LOG_TRACE_FMT("Initial accounts:");
  }

  void post_timing_body_hook() override
  {
    LOG_TRACE_FMT("Final accounts:");
  }

  void verify_params(const nlohmann::json& expected) override
  {
    Base::verify_params(expected);

    {
      const auto it = expected.find("accounts");
      if (it != expected.end())
      {
        const auto expected_accounts =
          it->get<decltype(options.total_accounts)>();
        if (expected_accounts != options.total_accounts)
        {
          if (!options.partition_clients)
          {
            throw std::runtime_error(
              "Verification file is only applicable for " +
              std::to_string(expected_accounts) +
              " accounts, but currently have " +
              std::to_string(options.total_accounts));
          }
        }
      }
    }
  }

  void verify_state(const std::string& prefix, const nlohmann::json& expected)
  {
    if (expected.is_null())
    {
      return;
    }

    auto expected_type_msg = [&prefix](const nlohmann::json& problematic) {
      return prefix +
        " state should be a list of (account, balance) objects, not: " +
        problematic.dump();
    };

    if (!expected.is_array())
    {
      throw std::runtime_error(expected_type_msg(expected));
    }

    // Create new connection to read balances
    auto conn = get_connection();

    for (const auto& entry : expected)
    {
      auto account_it = entry.find("account");
      auto balance_it = entry.find("balance");
      if (account_it == entry.end() || balance_it == entry.end())
      {
        throw std::runtime_error(expected_type_msg(entry));
      }

      BankSerializer bs(to_string(account_it->get<size_t>()));
      const auto response = conn->call("SmallBank_balance", bs.get_buffer());

      if (!http::status_success(response.status))
      {
        throw std::runtime_error(fmt::format(
          "Error in verification response: {}", conn->get_error(response)));
      }

      BalanceDeserializer bd(response.body.data());
      auto expected_balance = balance_it->get<int64_t>();
      auto actual_balance = bd.balance();
      if (expected_balance != actual_balance)
      {
        throw std::runtime_error(
          "Expected account " + account_it->dump() + " to have balance " +
          std::to_string(expected_balance) + ", actual balance is " +
          std::to_string(actual_balance));
      }
    }
  }

  void verify_initial_state(const nlohmann::json& expected) override
  {
    verify_state("Initial", expected);
  }

  void verify_final_state(const nlohmann::json& expected) override
  {
    verify_state("Final", expected);
  }

public:
  SmallBankClient(const SmallBankClientOptions& o) : Base(o) {
    from = 0;
    to = options.total_accounts;

    if (options.partition_clients)
    {
      from = options.client_id * options.total_accounts;
      to = from + options.total_accounts;
    }
  }
};

int main(int argc, char** argv)
{
  CLI::App cli_app{"Small Bank Client"};
  SmallBankClientOptions options(cli_app, argv[0]);
  CLI11_PARSE(cli_app, argc, argv);

  SmallBankClient client(options);
  client.run();

  return 0;
}
