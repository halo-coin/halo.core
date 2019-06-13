// Copyright (c) 2018-2019 Halo Sphere developers 
// Copyright (c) 2011-2016 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma  once

#include <future>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include "WalletRpcServerCommandsDefinitions.h"
#include "WalletLegacy/WalletLegacy.h"
#include "Common/CommandLine.h"
#include "Rpc/HttpServer.h"

#include <Logging/LoggerRef.h>

namespace Tools
{
  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  class wallet_rpc_server : CryptoNote::HttpServer
  {
  public:

    wallet_rpc_server(
      System::Dispatcher& dispatcher, 
      Logging::ILogger& log,
      CryptoNote::IWalletLegacy &w, 
      CryptoNote::INode &n, 
      CryptoNote::Currency& currency,
      const std::string& walletFilename);


    static void init_options(boost::program_options::options_description& desc);
    bool init(const boost::program_options::variables_map& vm);
    
    bool run();
    void send_stop_signal();

    static const command_line::arg_descriptor<uint16_t> arg_rpc_bind_port;
    static const command_line::arg_descriptor<std::string> arg_rpc_bind_ip;
    static const command_line::arg_descriptor<std::string> arg_rpc_user;
    static const command_line::arg_descriptor<std::string> arg_rpc_password;

  private:

    virtual void processRequest(const CryptoNote::HttpRequest& request, CryptoNote::HttpResponse& response) override;

    //json_rpc
    bool on_getbalance(const wallet_rpc::COMMAND_RPC_GET_BALANCE::request& req, wallet_rpc::COMMAND_RPC_GET_BALANCE::response& res);
    bool on_transfer(const wallet_rpc::COMMAND_RPC_TRANSFER::request& req, wallet_rpc::COMMAND_RPC_TRANSFER::response& res);
    bool on_store(const wallet_rpc::COMMAND_RPC_STORE::request& req, wallet_rpc::COMMAND_RPC_STORE::response& res);
    bool on_get_messages(const wallet_rpc::COMMAND_RPC_GET_MESSAGES::request& req, wallet_rpc::COMMAND_RPC_GET_MESSAGES::response& res);
    bool on_get_payments(const wallet_rpc::COMMAND_RPC_GET_PAYMENTS::request& req, wallet_rpc::COMMAND_RPC_GET_PAYMENTS::response& res);
    bool on_get_transfers(const wallet_rpc::COMMAND_RPC_GET_TRANSFERS::request& req, wallet_rpc::COMMAND_RPC_GET_TRANSFERS::response& res);
    bool on_get_transaction(const wallet_rpc::COMMAND_RPC_GET_TRANSACTION::request& req, wallet_rpc::COMMAND_RPC_GET_TRANSACTION::response& res);
    bool on_get_height(const wallet_rpc::COMMAND_RPC_GET_HEIGHT::request& req, wallet_rpc::COMMAND_RPC_GET_HEIGHT::response& res);
    bool on_query_key(const wallet_rpc::COMMAND_RPC_QUERY_KEY::request& req, wallet_rpc::COMMAND_RPC_QUERY_KEY::response& res);
    bool on_get_address(const wallet_rpc::COMMAND_RPC_GET_ADDRESS::request& req, wallet_rpc::COMMAND_RPC_GET_ADDRESS::response& res);
    bool on_stop_wallet(const wallet_rpc::COMMAND_RPC_STOP::request& req, wallet_rpc::COMMAND_RPC_STOP::response& res);
	  bool on_gen_paymentid(const wallet_rpc::COMMAND_RPC_GET_ADDRESS::request& req, wallet_rpc::COMMAND_RPC_GEN_PAYMENT_ID::response& res);
    bool on_get_tx_key(const wallet_rpc::COMMAND_RPC_GET_TX_KEY::request& req, wallet_rpc::COMMAND_RPC_GET_TX_KEY::response& res);
    bool on_get_tx_proof(const wallet_rpc::COMMAND_RPC_GET_TX_PROOF::request& req, wallet_rpc::COMMAND_RPC_GET_TX_PROOF::response& res);
	  bool on_get_reserve_proof(const wallet_rpc::COMMAND_RPC_GET_BALANCE_PROOF::request& req, wallet_rpc::COMMAND_RPC_GET_BALANCE_PROOF::response& res);
    bool on_sign_message(const wallet_rpc::COMMAND_RPC_SIGN_MESSAGE::request& req, wallet_rpc::COMMAND_RPC_SIGN_MESSAGE::response& res);
	  bool on_verify_message(const wallet_rpc::COMMAND_RPC_VERIFY_MESSAGE::request& req, wallet_rpc::COMMAND_RPC_VERIFY_MESSAGE::response& res);
    bool on_reset(const wallet_rpc::COMMAND_RPC_RESET::request& req, wallet_rpc::COMMAND_RPC_RESET::response& res);
    bool on_get_outputs(const wallet_rpc::COMMAND_RPC_GET_OUTPUTS::request& req, wallet_rpc::COMMAND_RPC_GET_OUTPUTS::response& res);

    bool handle_command_line(const boost::program_options::variables_map& vm);

    Logging::LoggerRef logger;
    CryptoNote::IWalletLegacy& m_wallet;
    CryptoNote::INode& m_node;
    uint16_t m_port;
    std::string m_bind_ip;
    std::string m_rpcUser;
    std::string m_rpcPassword;
    CryptoNote::Currency& m_currency;
    const std::string m_walletFilename;

    System::Dispatcher& m_dispatcher;
    System::Event m_stopComplete;
  };
}
