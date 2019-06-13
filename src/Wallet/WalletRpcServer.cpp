// Copyright (c) 2018-2019 Halo Sphere developers 
// Copyright (c) 2011-2016 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <list>
#include <boost/algorithm/string/predicate.hpp>
#include "WalletRpcServer.h"
#include "crypto/hash.h"
#include "Common/CommandLine.h"
#include "Common/StringTools.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/CryptoNoteBasicImpl.h"
#include "CryptoNoteCore/Account.h"
#include "Rpc/JsonRpc.h"
#include "WalletLegacy/WalletHelper.h"
#include "WalletLegacy/WalletLegacy.h"
#include "Common/StringTools.h"
#include "Common/Base58.h"
#include "Common/Util.h"

#include "ITransfersContainer.h"

using namespace Logging;
using namespace CryptoNote;

namespace Tools {

const command_line::arg_descriptor<uint16_t>    wallet_rpc_server::arg_rpc_bind_port = {"rpc-bind-port", "Starts wallet as rpc server for wallet operations, sets bind port for server", 0, true};
const command_line::arg_descriptor<std::string> wallet_rpc_server::arg_rpc_bind_ip   = {"rpc-bind-ip", "Specify ip to bind rpc server", "127.0.0.1"};
const command_line::arg_descriptor<std::string> wallet_rpc_server::arg_rpc_user      = {"rpc-user", "Username to use the rpc server. If authorization is not required, leave it empty", ""};
const command_line::arg_descriptor<std::string> wallet_rpc_server::arg_rpc_password  = {"rpc-password", "Password to use the rpc server. If authorization is not required, leave it empty", ""};

void wallet_rpc_server::init_options(boost::program_options::options_description& desc) {
  command_line::add_arg(desc, arg_rpc_bind_ip);
  command_line::add_arg(desc, arg_rpc_bind_port);
  command_line::add_arg(desc, arg_rpc_user);
  command_line::add_arg(desc, arg_rpc_password);
}
//------------------------------------------------------------------------------------------------------------------------------
wallet_rpc_server::wallet_rpc_server(
    System::Dispatcher&        dispatcher,
    Logging::ILogger&          log,
    CryptoNote::IWalletLegacy& w,
    CryptoNote::INode&         n,
    CryptoNote::Currency&      currency,
    const std::string&         walletFile)
    : HttpServer(dispatcher, log),
      logger(log, "WalletRpc"),
      m_dispatcher(dispatcher),
      m_stopComplete(dispatcher),
      m_wallet(w),
      m_node(n),
      m_currency(currency),
      m_walletFilename(walletFile) {
}
//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::run() {
  start(m_bind_ip, m_port, m_rpcUser, m_rpcPassword);
  m_stopComplete.wait();
  return true;
}

void wallet_rpc_server::send_stop_signal() {
  m_dispatcher.remoteSpawn([this] {
    std::cout << "wallet_rpc_server::send_stop_signal()" << std::endl;
    stop();
    m_stopComplete.set();
  });
}

//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::handle_command_line(const boost::program_options::variables_map& vm) {
    m_bind_ip     = command_line::get_arg(vm, arg_rpc_bind_ip);
    m_port        = command_line::get_arg(vm, arg_rpc_bind_port);
    m_rpcUser     = command_line::get_arg(vm, arg_rpc_user);
    m_rpcPassword = command_line::get_arg(vm, arg_rpc_password);
    return true;
}
//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::init(const boost::program_options::variables_map& vm) {
  if (!handle_command_line(vm)) {
    logger(ERROR) << "Failed to process command line in wallet_rpc_server";
    return false;
  }

  return true;
}

void wallet_rpc_server::processRequest(const CryptoNote::HttpRequest& request, CryptoNote::HttpResponse& response) {
    using namespace CryptoNote::JsonRpc;

    JsonRpcRequest  jsonRequest;
    JsonRpcResponse jsonResponse;

    try {
        jsonRequest.parseRequest(request.getBody());
        jsonResponse.setId(jsonRequest.getId());

        static std::unordered_map<std::string, JsonMemberMethod> s_methods = {
            {"getbalance", makeMemberMethod(&wallet_rpc_server::on_getbalance)},
            {"transfer", makeMemberMethod(&wallet_rpc_server::on_transfer)},
            {"store", makeMemberMethod(&wallet_rpc_server::on_store)},
            {"stop_wallet"  , makeMemberMethod(&wallet_rpc_server::on_stop_wallet)},
            {"get_paymentid", makeMemberMethod(&wallet_rpc_server::on_gen_paymentid)},
            {"get_messages", makeMemberMethod(&wallet_rpc_server::on_get_messages)},
            {"get_payments", makeMemberMethod(&wallet_rpc_server::on_get_payments)},
            {"get_transfers", makeMemberMethod(&wallet_rpc_server::on_get_transfers)},
            {"get_transaction", makeMemberMethod(&wallet_rpc_server::on_get_transaction) },
            {"get_height", makeMemberMethod(&wallet_rpc_server::on_get_height)},
            {"get_address", makeMemberMethod(&wallet_rpc_server::on_get_address)},
            {"query_key", makeMemberMethod(&wallet_rpc_server::on_query_key)},
            {"get_tx_proof", makeMemberMethod(&wallet_rpc_server::on_get_tx_proof)},
            {"get_reserve_proof", makeMemberMethod(&wallet_rpc_server::on_get_reserve_proof)},
            {"get_tx_key", makeMemberMethod(&wallet_rpc_server::on_get_tx_key)},
            {"get_outputs", makeMemberMethod(&wallet_rpc_server::on_get_outputs) },
            {"reset", makeMemberMethod(&wallet_rpc_server::on_reset)}};

        auto it = s_methods.find(jsonRequest.getMethod());
        if (it == s_methods.end()) {
            throw JsonRpcError(errMethodNotFound);
        }

        it->second(this, jsonRequest, jsonResponse);

    } catch (const JsonRpcError& err) {
        jsonResponse.setError(err);
    } catch (const std::exception& e) {
        jsonResponse.setError(JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, e.what()));
    }

    response.setBody(jsonResponse.getBody());
}

//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::on_getbalance(const wallet_rpc::COMMAND_RPC_GET_BALANCE::request& req, wallet_rpc::COMMAND_RPC_GET_BALANCE::response& res) {
  res.locked_amount = m_wallet.pendingBalance();
  res.available_balance = m_wallet.actualBalance();
  res.balance = res.locked_amount + res.available_balance;
  res.unlocked_balance = res.available_balance;
  return true;
}
//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::on_transfer(const wallet_rpc::COMMAND_RPC_TRANSFER::request& req, wallet_rpc::COMMAND_RPC_TRANSFER::response& res) {
  if (req.mixin < m_currency.minMixin() && req.mixin != 0) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_WRONG_MIXIN,
			std::string("Requested mixin \"" + std::to_string(req.mixin) + "\" is too low"));
	}
  std::vector<CryptoNote::WalletLegacyTransfer> transfers;
  std::vector<CryptoNote::TransactionMessage> messages;
  for (auto it = req.destinations.begin(); it != req.destinations.end(); it++) {
    CryptoNote::WalletLegacyTransfer transfer;
    transfer.address = it->address;
    transfer.amount = it->amount;
    transfers.push_back(transfer);

    if (!it->message.empty()) {
      messages.emplace_back(CryptoNote::TransactionMessage{ it->message, it->address });
    }
  }

  std::vector<uint8_t> extra;
  if (!req.payment_id.empty()) {
    std::string payment_id_str = req.payment_id;

    Crypto::Hash payment_id;
    if (!CryptoNote::parsePaymentId(payment_id_str, payment_id)) {
      throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID, 
        "Payment id has invalid format: \"" + payment_id_str + "\", expected 64-character string");
    }

    BinaryArray extra_nonce;
    CryptoNote::setPaymentIdToTransactionExtraNonce(extra_nonce, payment_id);
    if (!CryptoNote::addExtraNonceToTransactionExtra(extra, extra_nonce)) {
      throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID,
        "Something went wrong with payment_id. Please check its format: \"" + payment_id_str + "\", expected 64-character string");
    }
  }

  for (auto& rpc_message : req.messages) {
     messages.emplace_back(CryptoNote::TransactionMessage{ rpc_message.message, rpc_message.address });
  }

  uint64_t ttl = 0;
  if (req.ttl != 0) {
    ttl = static_cast<uint64_t>(time(nullptr)) + req.ttl;
  }

  std::string extraString;
  std::copy(extra.begin(), extra.end(), std::back_inserter(extraString));
  try {
    CryptoNote::WalletHelper::SendCompleteResultObserver sent;
    WalletHelper::IWalletRemoveObserverGuard removeGuard(m_wallet, sent);

    CryptoNote::TransactionId tx = m_wallet.sendTransaction(transfers, req.fee, extraString, req.mixin, req.unlock_time, messages, ttl);
    if (tx == WALLET_LEGACY_INVALID_TRANSACTION_ID) {
      throw std::runtime_error("Couldn't send transaction");
    }

    std::error_code sendError = sent.wait(tx);
    removeGuard.removeObserver();

    if (sendError) {
      throw std::system_error(sendError);
    }

    CryptoNote::WalletLegacyTransaction txInfo;
    m_wallet.getTransaction(tx, txInfo);
    res.tx_hash = Common::podToHex(txInfo.hash);

  } catch (const std::exception& e) {
    throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR, e.what());
  }
  return true;
}
//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::on_store(const wallet_rpc::COMMAND_RPC_STORE::request& req, wallet_rpc::COMMAND_RPC_STORE::response& res) {
  try {
    WalletHelper::storeWallet(m_wallet, m_walletFilename);
  } catch (std::exception& e) {
    throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Couldn't save wallet: ") + e.what());
  }

  return true;
}
//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::on_get_messages(const wallet_rpc::COMMAND_RPC_GET_MESSAGES::request& req, wallet_rpc::COMMAND_RPC_GET_MESSAGES::response& res) {
  res.total_tx_count = m_wallet.getTransactionCount();

  for (uint64_t i = req.first_tx_id; i < res.total_tx_count && res.tx_messages.size() < req.tx_limit; ++i) {
    WalletLegacyTransaction tx;
    if (!m_wallet.getTransaction(static_cast<TransactionId>(i), tx)) {
      throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, "Failed to get transaction");
    }

    if (!tx.messages.empty()) {
      wallet_rpc::transaction_messages tx_messages;
      tx_messages.tx_hash = Common::podToHex(tx.hash);
      tx_messages.tx_id = i;
      tx_messages.block_height = tx.blockHeight;
      tx_messages.timestamp = tx.timestamp;
      std::copy(tx.messages.begin(), tx.messages.end(), std::back_inserter(tx_messages.messages));

      res.tx_messages.emplace_back(std::move(tx_messages));
    }
  }

  return true;
}
//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::on_get_payments(const wallet_rpc::COMMAND_RPC_GET_PAYMENTS::request& req, 
  wallet_rpc::COMMAND_RPC_GET_PAYMENTS::response& res) {
  
	Crypto::Hash expectedPaymentId;
  CryptoNote::BinaryArray payment_id_blob;

  if (!Common::fromHex(req.payment_id, payment_id_blob)) 
    throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID, "Payment ID has invald format");
  if (sizeof(expectedPaymentId) != payment_id_blob.size()) 
    throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID, "Payment ID has invalid size");
  
  expectedPaymentId = *reinterpret_cast<const Crypto::Hash*>(payment_id_blob.data());
	size_t transactionsCount = m_wallet.getTransactionCount();
  
  // std::copy(std::begin(payment_id_blob), std::end(payment_id_blob), reinterpret_cast<char*>(&expectedPaymentId)); // no UB, char can alias any type
  // auto payments = m_wallet.getTransactionsByPaymentIds({expectedPaymentId});
  // assert(payments.size() == 1);

	for (size_t transactionNumber = 0; transactionNumber < transactionsCount; ++transactionNumber){
		WalletLegacyTransaction txInfo;
		m_wallet.getTransaction(transactionNumber, txInfo);
		if (txInfo.state != WalletLegacyTransactionState::Active || txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT)
			continue;
		if (txInfo.totalAmount < 0)
			continue;
		std::vector<uint8_t> extraVec;
		extraVec.reserve(txInfo.extra.size());
		std::for_each(txInfo.extra.begin(), txInfo.extra.end(), 
			[&extraVec](const char el) { extraVec.push_back(el); });

		Crypto::Hash paymentId;
		if (getPaymentIdFromTxExtra(extraVec, paymentId) && paymentId == expectedPaymentId)
		{
      wallet_rpc::payment_details rpc_payment;
      rpc_payment.tx_hash       = Common::podToHex(txInfo.hash);
      rpc_payment.amount        = txInfo.totalAmount;
      rpc_payment.block_height  = txInfo.blockHeight;
      rpc_payment.unlock_time   = txInfo.unlockTime;
      res.payments.push_back(rpc_payment);
    }
  }
  return true;
}

bool wallet_rpc_server::on_get_transfers(const wallet_rpc::COMMAND_RPC_GET_TRANSFERS::request& req, 
  wallet_rpc::COMMAND_RPC_GET_TRANSFERS::response& res) {
  res.transfers.clear();
  size_t transactionsCount = m_wallet.getTransactionCount();
  uint64_t bc_height;
	try {
		bc_height = m_node.getKnownBlockCount();
	}
	catch (std::exception &e) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Failed to get blockchain height: ") + e.what());
	}

  for (size_t transactionNumber = 0; transactionNumber < transactionsCount; ++transactionNumber) {
    WalletLegacyTransaction txInfo;
    m_wallet.getTransaction(transactionNumber, txInfo);
    // if (txInfo.state != WalletLegacyTransactionState::Active || txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
    if (txInfo.state == WalletLegacyTransactionState::Cancelled || txInfo.state == WalletLegacyTransactionState::Deleted 
			|| txInfo.state == WalletLegacyTransactionState::Failed)
      continue;
    // }

    std::string address = "";
    if (txInfo.totalAmount < 0 && txInfo.transferCount > 0) {
        WalletLegacyTransfer tr;
        m_wallet.getTransfer(txInfo.firstTransferId, tr);
        address = tr.address;
    }
    
    wallet_rpc::Transfer transfer;
    transfer.time = txInfo.timestamp;
    transfer.output = txInfo.totalAmount < 0;
    transfer.transactionHash = Common::podToHex(txInfo.hash);
    transfer.amount = std::abs(txInfo.totalAmount);
    transfer.fee = txInfo.fee;
    transfer.address = address;
    transfer.blockIndex = txInfo.blockHeight;
    transfer.unlockTime = txInfo.unlockTime;
    transfer.paymentId = "";
		transfer.confirmations = (txInfo.blockHeight != UNCONFIRMED_TRANSACTION_GLOBAL_OUTPUT_INDEX ? bc_height - txInfo.blockHeight : 0);

    std::vector<uint8_t> extraVec;
    extraVec.reserve(txInfo.extra.size());
    std::for_each(txInfo.extra.begin(), txInfo.extra.end(), [&extraVec](const char el) { extraVec.push_back(el); });

    Crypto::Hash paymentId;
    transfer.paymentId = (getPaymentIdFromTxExtra(extraVec, paymentId) && paymentId != NULL_HASH ? Common::podToHex(paymentId) : "");
    transfer.txKey = (txInfo.secretKey != NULL_SECRET_KEY ? Common::podToHex(txInfo.secretKey) : "");

    res.transfers.push_back(transfer);
  }

  return true;
}

bool wallet_rpc_server::on_get_transaction(const wallet_rpc::COMMAND_RPC_GET_TRANSACTION::request& req,
	wallet_rpc::COMMAND_RPC_GET_TRANSACTION::response& res)
{
	res.destinations.clear();
	uint64_t bc_height;
	try {
		bc_height = m_node.getKnownBlockCount();
	}
	catch (std::exception &e) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Failed to get blockchain height: ") + e.what());
	}

	size_t transactionsCount = m_wallet.getTransactionCount();
	for (size_t transactionNumber = 0; transactionNumber < transactionsCount; ++transactionNumber)
	{
		WalletLegacyTransaction txInfo;
		m_wallet.getTransaction(transactionNumber, txInfo);
		if (txInfo.state == WalletLegacyTransactionState::Cancelled || txInfo.state == WalletLegacyTransactionState::Deleted
			|| txInfo.state == WalletLegacyTransactionState::Failed)
			continue;

		if (boost::iequals(Common::podToHex(txInfo.hash), req.tx_hash))
		{
			std::string address = "";
			if (txInfo.totalAmount < 0 && txInfo.transferCount > 0)
			{
				WalletLegacyTransfer ftr;
				m_wallet.getTransfer(txInfo.firstTransferId, ftr);
				address = ftr.address;
			}

			wallet_rpc::Transfer transfer;
			transfer.time = txInfo.timestamp;
			transfer.output = txInfo.totalAmount < 0;
			transfer.transactionHash = Common::podToHex(txInfo.hash);
			transfer.amount = std::abs(txInfo.totalAmount);
			transfer.fee = txInfo.fee;
			transfer.address = address;
			transfer.blockIndex = txInfo.blockHeight;
			transfer.unlockTime = txInfo.unlockTime;
			transfer.paymentId = "";
			transfer.confirmations = (txInfo.blockHeight != UNCONFIRMED_TRANSACTION_GLOBAL_OUTPUT_INDEX ? bc_height - txInfo.blockHeight : 0);
			
			std::vector<uint8_t> extraVec;
			extraVec.reserve(txInfo.extra.size());
			std::for_each(txInfo.extra.begin(), txInfo.extra.end(), [&extraVec](const char el) { extraVec.push_back(el); });

			Crypto::Hash paymentId;
			transfer.paymentId = (getPaymentIdFromTxExtra(extraVec, paymentId) && paymentId != NULL_HASH ? Common::podToHex(paymentId) : "");

			transfer.txKey = (txInfo.secretKey != NULL_SECRET_KEY ? Common::podToHex(txInfo.secretKey) : "");

			res.transaction_details = transfer;

			for (TransferId id = txInfo.firstTransferId; id < txInfo.firstTransferId + txInfo.transferCount; ++id) {
				WalletLegacyTransfer txtr;
				m_wallet.getTransfer(id, txtr);
				wallet_rpc::transfer_destination dest;
				dest.amount = txtr.amount;
				dest.address = txtr.address;
				res.destinations.push_back(dest);
			}
			return true;
		}
	}

	throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR,
		std::string("Transaction with this hash not found: ") + req.tx_hash);

	return false;
}

bool wallet_rpc_server::on_get_height(const wallet_rpc::COMMAND_RPC_GET_HEIGHT::request& req, wallet_rpc::COMMAND_RPC_GET_HEIGHT::response& res) {
  res.height = m_node.getLastLocalBlockHeight();
  return true;
}

bool wallet_rpc_server::on_get_address(const wallet_rpc::COMMAND_RPC_GET_ADDRESS::request& req, wallet_rpc::COMMAND_RPC_GET_ADDRESS::response& res) {
  res.address = m_wallet.getAddress();
  return true;
}

bool wallet_rpc_server::on_reset(const wallet_rpc::COMMAND_RPC_RESET::request& req, wallet_rpc::COMMAND_RPC_RESET::response& res) {
  m_wallet.reset();
  return true;
}

bool wallet_rpc_server::on_query_key(const wallet_rpc::COMMAND_RPC_QUERY_KEY::request& req, wallet_rpc::COMMAND_RPC_QUERY_KEY::response& res) {
    if (0 != req.key_type.compare("mnemonic") && 0 != req.key_type.compare("paperwallet"))
        throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Unsupported key_type ") + req.key_type);
    if (0 == req.key_type.compare("mnemonic") && !m_wallet.getSeed(res.key))
        throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("The wallet is non-deterministic. Cannot display seed."));
    if (0 == req.key_type.compare("paperwallet")) {
        AccountKeys keys;
        m_wallet.getAccountKeys(keys);
        res.key = Tools::Base58::encode_addr(parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX,
                                             std::string(reinterpret_cast<char*>(&keys), sizeof(keys)));
    }
    return true;
}


bool wallet_rpc_server::on_get_tx_proof(const wallet_rpc::COMMAND_RPC_GET_TX_PROOF::request& req,
	wallet_rpc::COMMAND_RPC_GET_TX_PROOF::response& res) {
	Crypto::Hash txid;
	if (!parse_hash256(req.tx_hash, txid)) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Failed to parse tx_hash"));
	}
	CryptoNote::AccountPublicAddress dest_address;
	if (!m_currency.parseAccountAddressString(req.dest_address, dest_address)) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_WRONG_ADDRESS, std::string("Failed to parse address"));
	}

	Crypto::SecretKey tx_key, tx_key2;
	bool r = m_wallet.get_tx_key(txid, tx_key);

	if (!req.tx_key.empty()) {
		Crypto::Hash tx_key_hash;
		size_t size;
		if (!Common::fromHex(req.tx_key, &tx_key_hash, sizeof(tx_key_hash), size) || size != sizeof(tx_key_hash)) {
			throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Failed to parse tx_key"));
		}
		tx_key2 = *(struct Crypto::SecretKey *) &tx_key_hash;

		if (r) {
			if (tx_key != tx_key2) {
				throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, 
					std::string("Tx secret key was found for the given txid, but you've also provided another tx secret key which doesn't match the found one."));
			}
		}
		tx_key = tx_key2;
	}
	else {
		if (!r) {
			throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR,
				std::string("Tx secret key wasn't found in the wallet file. Provide it as the optional <tx_key> parameter if you have it elsewhere."));
		}
	}
	
	std::string sig_str;
	if (m_wallet.getTxProof(txid, dest_address, tx_key, sig_str)) {
		res.signature = sig_str;
	}
	else {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Failed to get transaction proof"));
	}

	return true;
}

bool wallet_rpc_server::on_get_reserve_proof(const wallet_rpc::COMMAND_RPC_GET_BALANCE_PROOF::request& req,
	wallet_rpc::COMMAND_RPC_GET_BALANCE_PROOF::response& res) {

	if (m_wallet.isTrackingWallet()) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("This is tracking wallet. The reserve proof can be generated only by a full wallet."));
	}

	try {
		res.signature = m_wallet.getReserveProof(req.amount != 0 ? req.amount : m_wallet.actualBalance(), !req.message.empty() ? req.message : "");
	}
	catch (const std::exception &e) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, e.what());
	}

	return true;
}

bool wallet_rpc_server::on_get_tx_key(const wallet_rpc::COMMAND_RPC_GET_TX_KEY::request& req,
	wallet_rpc::COMMAND_RPC_GET_TX_KEY::response& res) {
	Crypto::Hash txid;
	if (!parse_hash256(req.tx_hash, txid)) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Failed to parse txid"));
	}

	Crypto::SecretKey tx_key = m_wallet.getTxKey(txid);
	if (tx_key != NULL_SECRET_KEY) {
		res.tx_key = Common::podToHex(tx_key);
	}
	else {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("No tx key found for this txid"));
	}
	return true;
}

//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::on_stop_wallet(const wallet_rpc::COMMAND_RPC_STOP::request& req, wallet_rpc::COMMAND_RPC_STOP::response& res) {
	try {
		WalletHelper::storeWallet(m_wallet, m_walletFilename);
	}
	catch (std::exception& e) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Couldn't save wallet: ") + e.what());
	}
	wallet_rpc_server::send_stop_signal();
	return true;
}
//------------------------------------------------------------------------------------------------------------------------------

bool wallet_rpc_server::on_gen_paymentid(const wallet_rpc::COMMAND_RPC_GEN_PAYMENT_ID::request& req, wallet_rpc::COMMAND_RPC_GEN_PAYMENT_ID::response& res)
{
	std::string pid;
	try {
		pid = Common::podToHex(Crypto::rand<Crypto::Hash>());
	}
	catch (const std::exception& e) {
		throw JsonRpc::JsonRpcError(WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR, std::string("Internal error: can't generate Payment ID: ") + e.what());
	}
	res.payment_id = pid;
	return true;
}

//------------------------------------------------------------------------------------------------------------------------------
bool wallet_rpc_server::on_get_outputs(const wallet_rpc::COMMAND_RPC_GET_OUTPUTS::request& req, wallet_rpc::COMMAND_RPC_GET_OUTPUTS::response& res) {
  std::vector<TransactionOutputInformation> outputs = m_wallet.getUnlockedOutputs();
  uint64_t total = 0;
  for (auto output : outputs) {
    wallet_rpc::outputs_details o_details;
    o_details.tx_hash = Common::podToHex(output.transactionHash);
    o_details.amount = output.amount;
    total += output.amount;
    res.outputs.push_back(o_details);
  }
  res.unlocked_outputs_count = outputs.size();
  res.total = total;

  return true;
}

}
