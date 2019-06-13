// Copyright (c) 2018-2019 Halo Sphere developers 
// Copyright (c) 2011-2016 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "SimpleWallet.h"

#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <thread>
#include <set>
#include <sstream>

#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "Common/CommandLine.h"
#include "Common/SignalHandler.h"
#include "Common/StringTools.h"
#include <Common/Base58.h>
#include "Common/PathTools.h"
#include "Common/DnsTools.h"
#include "Common/UrlTools.h"
#include "Common/Util.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include "Rpc/CoreRpcServerCommandsDefinitions.h"
#include "Rpc/HttpClient.h"

#include "Wallet/WalletRpcServer.h"
#include "WalletLegacy/WalletLegacy.h"
#include "Wallet/LegacyKeysImporter.h"
#include "WalletLegacy/WalletHelper.h"
#include "Mnemonics/electrum-words.h"
#include "CryptoNoteCore/CryptoNoteTools.h"

#include "version.h"

#include <Logging/LoggerManager.h>

#if defined(WIN32)
#include <crtdbg.h>
#endif

using namespace CryptoNote;
using namespace Logging;
using Common::JsonValue;

namespace po = boost::program_options;

#define EXTENDED_LOGS_FILE "wallet_details.log"
#undef ERROR

namespace {

const command_line::arg_descriptor<std::string> arg_wallet_file = { "wallet-file", "Use wallet <arg>", "" };
const command_line::arg_descriptor<std::string> arg_generate_new_wallet = { "generate-new-wallet", "Generate new wallet and save it to <arg>", "" };
const command_line::arg_descriptor<std::string> arg_daemon_address = { "daemon-address", "Use daemon instance at <host>:<port>", "" };
const command_line::arg_descriptor<std::string> arg_daemon_host = { "daemon-host", "Use daemon instance at host <arg> instead of localhost", "" };
const command_line::arg_descriptor<std::string> arg_password = { "password", "Wallet password", "", true };
const command_line::arg_descriptor<std::string> arg_mnemonic_seed = { "mnemonic-seed", "Specify mnemonic seed for wallet recovery/creation", "" };
const command_line::arg_descriptor<bool> arg_restore_deterministic_wallet = { "restore-deterministic-wallet", "Recover wallet using electrum-style mnemonic", false };
const command_line::arg_descriptor<bool> arg_non_deterministic = { "non-deterministic", "Creates non-deterministic (classic) view and spend keys", false };
const command_line::arg_descriptor<uint16_t> arg_daemon_port = { "daemon-port", "Use daemon instance at port <arg> instead of 8081", 0 };
const command_line::arg_descriptor<uint32_t> arg_log_level = { "set_log", "", INFO, true };
const command_line::arg_descriptor<bool> arg_testnet = { "testnet", "Used to deploy test nets. The daemon must be launched with --testnet flag", false };
const command_line::arg_descriptor< std::vector<std::string> > arg_command = { "command", "" };


const size_t TIMESTAMP_MAX_WIDTH = 19;
const size_t HASH_MAX_WIDTH = 64;
const size_t TOTAL_AMOUNT_MAX_WIDTH = 20;
const size_t FEE_MAX_WIDTH = 14;
const size_t BLOCK_MAX_WIDTH = 7;
const size_t UNLOCK_TIME_MAX_WIDTH = 11;

//----------------------------------------------------------------------------------------------------
bool parseUrlAddress(const std::string& url, std::string& address, uint16_t& port) {
  auto pos = url.find("://");
  size_t addrStart = 0;

  if (pos != std::string::npos) {
    addrStart = pos + 3;
  }

  auto addrEnd = url.find(':', addrStart);

  if (addrEnd != std::string::npos) {
    auto portEnd = url.find('/', addrEnd);
    port = Common::fromString<uint16_t>(url.substr(
      addrEnd + 1, portEnd == std::string::npos ? std::string::npos : portEnd - addrEnd - 1));
  } else {
    addrEnd = url.find('/');
    port = 80;
  }

  address = url.substr(addrStart, addrEnd - addrStart);
  return true;
}

//----------------------------------------------------------------------------------------------------
inline std::string interpret_rpc_response(bool ok, const std::string& status) {
  std::string err;
  if (ok) {
    if (status == CORE_RPC_STATUS_BUSY) {
      err = "daemon is busy. Please try later";
    } else if (status != CORE_RPC_STATUS_OK) {
      err = status;
    }
  } else {
    err = "possible lost connection to daemon";
  }
  return err;
}

template <typename IterT, typename ValueT = typename IterT::value_type>
class ArgumentReader {
public:

  ArgumentReader(IterT begin, IterT end) :
    m_begin(begin), m_end(end), m_cur(begin) {
  }

  bool eof() const {
    return m_cur == m_end;
  }

  ValueT next() {
    if (eof()) {
      throw std::runtime_error("unexpected end of arguments");
    }

    return *m_cur++;
  }

private:

  IterT m_cur;
  IterT m_begin;
  IterT m_end;
};

struct TransferCommand {
  const CryptoNote::Currency& m_currency;
  size_t fake_outs_count;
  std::vector<CryptoNote::WalletLegacyTransfer> dsts;
  std::vector<uint8_t> extra;
  uint64_t fee;
  std::map<std::string, std::vector<WalletLegacyTransfer>> aliases;
  std::vector<std::string> messages;
  uint64_t ttl;

  TransferCommand(const CryptoNote::Currency& currency) :
    m_currency(currency), fake_outs_count(0), fee(currency.minimumFee()), ttl(0) {
  }

  bool parseArguments(LoggerRef& logger, const std::vector<std::string> &args) {
    ArgumentReader<std::vector<std::string>::const_iterator> ar(args.begin(), args.end());

    try {
      auto mixin_str = ar.next();

      if (!Common::fromString(mixin_str, fake_outs_count)) {
        logger(ERROR, BRIGHT_RED) << "Mixin count should be non-negative integer, got " << mixin_str;
        return false;
      }

	    if (fake_outs_count < m_currency.minMixin() && fake_outs_count != 0) {
          logger(ERROR, BRIGHT_RED) << "Mixin should be equal to or bigger than " << m_currency.minMixin();
          return false;
      }

      if (fake_outs_count > m_currency.maxMixin()) {
          logger(ERROR, BRIGHT_RED) << "Mixin should be equal to or less than " << m_currency.maxMixin();
          return false;
      }

      bool feeFound = false;
      bool ttlFound = false;
      while (!ar.eof()) {
        auto arg = ar.next();
        if (arg.size() && arg[0] == '-') {
          const auto& value = ar.next();

          if (arg == "-p") {
            if (!createTxExtraWithPaymentId(value, extra)) {
              logger(ERROR, BRIGHT_RED) << "payment ID has invalid format: \"" << value << "\", expected 64-character string";
              return false;
            }
          } else if (arg == "-f") {
            feeFound = true;

            if (ttlFound) {
              logger(ERROR, BRIGHT_RED) << "Transaction with TTL can not have fee";
              return false;
            }

            bool ok = m_currency.parseAmount(value, fee);
            if (!ok) {
              logger(ERROR, BRIGHT_RED) << "Fee value is invalid: " << value;
              return false;
            }

            if (fee < m_currency.minimumFee()) {
              logger(ERROR, BRIGHT_RED) << "Fee value is less than minimum: " << m_currency.minimumFee();
              return false;
            }
          } else if (arg == "-m") {
            messages.emplace_back(value);
          } else if (arg == "-ttl") {
            ttlFound = true;

            if (feeFound) {
              logger(ERROR, BRIGHT_RED) << "Transaction with fee can not have TTL";
              return false;
            } else {
              fee = 0;
            }

            if (!Common::fromString(value, ttl) || ttl < 1 || ttl * 60 > m_currency.mempoolTxLiveTime()) {
              logger(ERROR, BRIGHT_RED) << "TTL has invalid format: \"" << value << "\", " <<
                "enter time from 1 to " << (m_currency.mempoolTxLiveTime() / 60) << " minutes";
              return false;
            }
          }
        } else {
            if (arg.length() == 187) {
                std::string paymentID;
                std::string spendPublicKey;
                std::string viewPublicKey;
                const uint64_t paymentIDLen = 64;

                /* extract the payment id */
                std::string decoded;
                uint64_t prefix;
                if (Tools::Base58::decode_addr(arg, prefix, decoded)) {
                    paymentID = decoded.substr(0, paymentIDLen);
                }

                /* validate and add the payment ID to extra */
                if (!createTxExtraWithPaymentId(paymentID, extra)) {
                    logger(ERROR, BRIGHT_RED) << "Integrated payment ID has invalid format: \"" << paymentID << "\", expected 64-character string";
                    return false;
                }

                /* create the address from the public keys */
                std::string keys = decoded.substr(paymentIDLen, std::string::npos);
                CryptoNote::AccountPublicAddress addr;
                CryptoNote::BinaryArray ba = Common::asBinaryArray(keys);

                if (!CryptoNote::fromBinaryArray(addr, ba)) {
                    return true;
                }

                std::string address = CryptoNote::getAccountAddressAsStr(CryptoNote::parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX, addr);

                arg = address;
            }
            WalletLegacyTransfer destination;
            CryptoNote::TransactionDestinationEntry de;
            std::string aliasUrl;

            if (!m_currency.parseAccountAddressString(arg, de.addr)) {
                Crypto::Hash paymentId;
                if (CryptoNote::parsePaymentId(arg, paymentId)) {
                    logger(ERROR, BRIGHT_RED) << "Invalid payment ID usage. Please, use -p <payment_id>. See help for details.";
                } else {
                    // if string doesn't contain a dot, we won't consider it a url for now.
                    if (strchr(arg.c_str(), '.') == NULL) {
                        logger(ERROR, BRIGHT_RED) << "Wrong address or alias: " << arg;
                        return false;
                    }
                    aliasUrl = arg;
                }
            }

            auto value = ar.next();
            bool ok = m_currency.parseAmount(value, de.amount);
            if (!ok || 0 == de.amount) {
                logger(ERROR, BRIGHT_RED) << "amount is wrong: " << arg << ' ' << value << ", expected number from 0 to "
                                          << m_currency.formatAmount(std::numeric_limits<uint64_t>::max());
                return false;
          }

          if (aliasUrl.empty()) {
            destination.address = arg;
            destination.amount = de.amount;
            dsts.push_back(destination);
          } else {
            aliases[aliasUrl].emplace_back(WalletLegacyTransfer{"", static_cast<int64_t>(de.amount)});
          }
          if (!remote_fee_address.empty()) {
              destination.address = remote_fee_address;
              int64_t remote_node_fee = static_cast<int64_t>(de.amount * CryptoNote::parameters::REMOTE_NODE_MIN_FEE);
              if (remote_node_fee > CryptoNote::parameters::REMOTE_NODE_MAX_FEE)
                  remote_node_fee = CryptoNote::parameters::REMOTE_NODE_MAX_FEE;
              destination.amount = remote_node_fee;
              dsts.push_back(destination);
          }
        }
      }

      if (dsts.empty() && aliases.empty()) {
        logger(ERROR, BRIGHT_RED) << "At least one destination address is required";
        return false;
      }
    } catch (const std::exception& e) {
      logger(ERROR, BRIGHT_RED) << e.what();
      return false;
    }

    return true;
  }
};

//----------------------------------------------------------------------------------------------------
JsonValue buildLoggerConfiguration(Level level, const std::string& logfile) {
  JsonValue loggerConfiguration(JsonValue::OBJECT);
  loggerConfiguration.insert("globalLevel", static_cast<int64_t>(level));

  JsonValue& cfgLoggers = loggerConfiguration.insert("loggers", JsonValue::ARRAY);

  JsonValue& consoleLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  consoleLogger.insert("type", "console");
  consoleLogger.insert("level", static_cast<int64_t>(TRACE));
  consoleLogger.insert("pattern", "%D %T %L ");

  JsonValue& fileLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  fileLogger.insert("type", "file");
  fileLogger.insert("filename", logfile);
  fileLogger.insert("level", static_cast<int64_t>(TRACE));

  return loggerConfiguration;
}

//----------------------------------------------------------------------------------------------------
std::error_code initAndLoadWallet(IWalletLegacy& wallet, std::istream& walletFile, const std::string& password) {
  WalletHelper::InitWalletResultObserver initObserver;
  std::future<std::error_code> f_initError = initObserver.initResult.get_future();

  WalletHelper::IWalletRemoveObserverGuard removeGuard(wallet, initObserver);
  wallet.initAndLoad(walletFile, password);
  auto initError = f_initError.get();

  return initError;
}

//----------------------------------------------------------------------------------------------------
std::string tryToOpenWalletOrLoadKeysOrThrow(LoggerRef& logger, std::unique_ptr<IWalletLegacy>& wallet, const std::string& walletFile, const std::string& password) {
  std::string keys_file, walletFileName;
  WalletHelper::prepareFileNames(walletFile, keys_file, walletFileName);

  boost::system::error_code ignore;
  bool keysExists = boost::filesystem::exists(keys_file, ignore);
  bool walletExists = boost::filesystem::exists(walletFileName, ignore);
  if (!walletExists && !keysExists && boost::filesystem::exists(walletFile, ignore)) {
    boost::system::error_code renameEc;
    boost::filesystem::rename(walletFile, walletFileName, renameEc);
    if (renameEc) {
      throw std::runtime_error("failed to rename file '" + walletFile + "' to '" + walletFileName + "': " + renameEc.message());
    }

    walletExists = true;
  }

  if (walletExists) {
    logger(INFO) << "Loading wallet...";
    std::ifstream walletFile;
    walletFile.open(walletFileName, std::ios_base::binary | std::ios_base::in);
    if (walletFile.fail()) {
      throw std::runtime_error("error opening wallet file '" + walletFileName + "'");
    }

    auto initError = initAndLoadWallet(*wallet, walletFile, password);

    walletFile.close();
    if (initError) { //bad password, or legacy format
      if (keysExists) {
        std::stringstream ss;
        CryptoNote::importLegacyKeys(keys_file, password, ss);
        boost::filesystem::rename(keys_file, keys_file + ".back");
        boost::filesystem::rename(walletFileName, walletFileName + ".back");

        initError = initAndLoadWallet(*wallet, ss, password);
        if (initError) {
          throw std::runtime_error("failed to load wallet: " + initError.message());
        }

        logger(INFO) << "Storing wallet...";

        try {
          CryptoNote::WalletHelper::storeWallet(*wallet, walletFileName);
        } catch (std::exception& e) {
          logger(ERROR, BRIGHT_RED) << "Failed to store wallet: " << e.what();
          throw std::runtime_error("error saving wallet file '" + walletFileName + "'");
        }

        logger(INFO, BRIGHT_GREEN) << "Stored ok";
        return walletFileName;
      } else { // no keys, wallet error loading
        throw std::runtime_error("can't load wallet file '" + walletFileName + "', check password");
      }
    } else { //new wallet ok 
      return walletFileName;
    }
  } else if (keysExists) { //wallet not exists but keys presented
    std::stringstream ss;
    CryptoNote::importLegacyKeys(keys_file, password, ss);
    boost::filesystem::rename(keys_file, keys_file + ".back");

    WalletHelper::InitWalletResultObserver initObserver;
    std::future<std::error_code> f_initError = initObserver.initResult.get_future();

    WalletHelper::IWalletRemoveObserverGuard removeGuard(*wallet, initObserver);
    wallet->initAndLoad(ss, password);
    auto initError = f_initError.get();

    removeGuard.removeObserver();
    if (initError) {
      throw std::runtime_error("failed to load wallet: " + initError.message());
    }

    logger(INFO) << "Storing wallet...";

    try {
      CryptoNote::WalletHelper::storeWallet(*wallet, walletFileName);
    } catch(std::exception& e) {
      logger(ERROR, BRIGHT_RED) << "Failed to store wallet: " << e.what();
      throw std::runtime_error("error saving wallet file '" + walletFileName + "'");
    }

    logger(INFO, BRIGHT_GREEN) << "Stored ok";
    return walletFileName;
  } else { //no wallet no keys
    throw std::runtime_error("wallet file '" + walletFileName + "' is not found");
  }
}

//----------------------------------------------------------------------------------------------------
std::string makeCenteredString(size_t width, const std::string& text) {
  if (text.size() >= width) {
    return text;
  }

  size_t offset = (width - text.size() + 1) / 2;
  return std::string(offset, ' ') + text + std::string(width - text.size() - offset, ' ');
}

//----------------------------------------------------------------------------------------------------
void printListTransfersHeader(LoggerRef& logger) {
  std::string header = makeCenteredString(TIMESTAMP_MAX_WIDTH, "TIMESTAMP (UTC)") + "  ";
  header += makeCenteredString(HASH_MAX_WIDTH, "HASH") + "  ";
  header += makeCenteredString(TOTAL_AMOUNT_MAX_WIDTH, "TOTAL AMOUNT") + "  ";
  header += makeCenteredString(FEE_MAX_WIDTH, "FEE") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "BLOCK") + "  ";
  header += makeCenteredString(UNLOCK_TIME_MAX_WIDTH, "UNLOCK TIME");

  logger(INFO) << std::string(header.size(), '-');
  logger(INFO) << header;
 // logger(INFO) << std::string(header.size(), '-');  logger(INFO) << header;
  logger(INFO) << std::string(header.size(), '-');
}

//----------------------------------------------------------------------------------------------------
void printListTransfersItem(LoggerRef& logger, const WalletLegacyTransaction& txInfo, IWalletLegacy& wallet, const Currency& currency) {
  std::vector<uint8_t> extraVec = Common::asBinaryArray(txInfo.extra);

  Crypto::Hash paymentId;
  std::string paymentIdStr = (getPaymentIdFromTxExtra(extraVec, paymentId) && paymentId != NULL_HASH ? Common::podToHex(paymentId) : "");

  char timeString[TIMESTAMP_MAX_WIDTH + 1];
  time_t timestamp = static_cast<time_t>(txInfo.timestamp);
  if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", std::gmtime(&timestamp)) == 0) {
    throw std::runtime_error("time buffer is too small");
  }

  std::string txHash;
  if (static_cast<int64_t>(txInfo.depositCount) > 0) {
      txHash = " ^" + Common::podToHex(txInfo.hash);
  } else {
      txHash = "  " + Common::podToHex(txInfo.hash);
  }

  std::string rowColor = txInfo.totalAmount < 0 ? MAGENTA : GREEN;
  logger(INFO, rowColor)
    << std::setw(TIMESTAMP_MAX_WIDTH) << timeString
    << "" << std::setw(HASH_MAX_WIDTH) << txHash
    << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << currency.formatAmount(txInfo.totalAmount)
    << "  " << std::setw(FEE_MAX_WIDTH) << currency.formatAmount(txInfo.fee)
    << "  " << std::setw(BLOCK_MAX_WIDTH) << txInfo.blockHeight
    << "  " << std::setw(UNLOCK_TIME_MAX_WIDTH) << txInfo.unlockTime;

  if (!paymentIdStr.empty()) {
    logger(INFO, CYAN) 
      << std::setw(TIMESTAMP_MAX_WIDTH) << "PAYMENT ID"
      << "  " << std::setw(HASH_MAX_WIDTH) << paymentIdStr;
  }

  if (txInfo.totalAmount < 0) {
    if (txInfo.transferCount > 0) {
      logger(INFO, rowColor) 
         << std::setw(TIMESTAMP_MAX_WIDTH) << "TRANSFERS";
      for (TransferId id = txInfo.firstTransferId; id < txInfo.firstTransferId + txInfo.transferCount; ++id) {
        WalletLegacyTransfer tr;
        wallet.getTransfer(id, tr);
        logger(INFO, BLUE) 
          << std::setw(TIMESTAMP_MAX_WIDTH) 
          << "  " << tr.address;
        logger(INFO, rowColor) 
          << std::setw(TIMESTAMP_MAX_WIDTH) << "  " 
          << "  " << std::setw(HASH_MAX_WIDTH) << "  "
          << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << std::right << currency.formatAmount(tr.amount);
      }
    }
  }

  // logger(INFO, rowColor) << " "; //just to make logger print one endline
}

//----------------------------------------------------------------------------------------------------
std::string prepareWalletAddressFilename(const std::string& walletBaseName) {
  return walletBaseName + ".address";
}

//----------------------------------------------------------------------------------------------------
bool writeAddressFile(const std::string& addressFilename, const std::string& address) {
  std::ofstream addressFile(addressFilename, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!addressFile.good()) {
    return false;
  }

  addressFile << address;

  return true;
}

//----------------------------------------------------------------------------------------------------
bool processServerAliasResponse(const std::string& response, std::string& address) {
	try {

		// Courtesy of Monero Project
		auto pos = response.find("oa1:halo");
		if (pos == std::string::npos)
			return false;
		// search from there to find "recipient_address="
		pos = response.find("recipient_address=", pos);
		if (pos == std::string::npos)
			return false;
		pos += 18; // move past "recipient_address="
		// find the next semicolon
		auto pos2 = response.find(";", pos);
		if (pos2 != std::string::npos)
		{
			// length of address == 98, we can at least validate that much here
			if (pos2 - pos == 98)
			{
				address = response.substr(pos, 98);
			} else {
				return false;
			}
		}
	}
	catch (std::exception&) {
		return false;
	}

	return true;
}

//----------------------------------------------------------------------------------------------------
bool processServerFeeAddressResponse(const std::string& response, std::string& fee_address) {
    try {
        std::stringstream stream(response);
        JsonValue json;
        stream >> json;

        auto rootIt = json.getObject().find("fee_address");
        if (rootIt == json.getObject().end()) {
            return false;
        }

        fee_address = rootIt->second.getString();
    }
    catch (std::exception&) {
        return false;
    }

    return true;
}

//----------------------------------------------------------------------------------------------------
bool askAliasesTransfersConfirmation(const std::map<std::string, std::vector<WalletLegacyTransfer>>& aliases, const Currency& currency, LoggerRef logger) {
  logger(INFO, CYAN) << "Resolved addresses list ... " << std::endl;

  for (const auto& kv: aliases) {
    for (const auto& transfer: kv.second) {
      logger(INFO, GREEN) << transfer.address << " " << std::setw(21) << currency.formatAmount(transfer.amount) << "  " << kv.first << std::endl;
    }
  }

  std::string answer;
  char ans;
  bool confirm = false;
  do {
    logger(INFO, RED) << "Are you sure, would you like to proceed.. ? y/n # ";
    std::getline(std::cin, answer);
    ans = answer[0];
    if ( ans == 'y' || ans == 'Y' || ans == 'n' || ans == 'N' ){
      confirm = true;
    }
  } while( !confirm );

  return ans == 'y' || ans == 'Y';
}

}

//----------------------------------------------------------------------------------------------------
std::string simple_wallet::get_commands_str() {
  std::stringstream ss;
  ss << "Commands: " << ENDL;
  std::string usage = m_consoleHandler.getUsage();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << ENDL;
  return ss.str();
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::help(const std::vector<std::string> &args/* = std::vector<std::string>()*/) {
  success_msg_writer() << get_commands_str();
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::seed(const std::vector<std::string> &args/* = std::vector<std::string>()*/) {
  std::string electrum_words;
  bool success = m_wallet->getSeed(electrum_words);

  if (success)
  {
    logger(INFO, RED) << "Please write down following 25 words and keep them secure; Don't share with anyone;";
    // std::cout << "\nPLEASE NOTE: the following 25 words can be used to recover access to your wallet. Please write them down and store them somewhere safe and secure. Please do not store them in your email or on file storage services outside of your immediate control.\n";
    logger(INFO, BRIGHT_GREEN) << electrum_words;
  }
  else 
  {
    fail_msg_writer() << "The wallet is non-deterministic and doesn't have mnemonic seed.";
  }
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::exit(const std::vector<std::string> &args) {
  m_consoleHandler.requestStop();
  return true;
}

//----------------------------------------------------------------------------------------------------
simple_wallet::simple_wallet(System::Dispatcher& dispatcher, const CryptoNote::Currency& currency, Logging::LoggerManager& log) :
  m_dispatcher(dispatcher),
  m_daemon_port(0), 
  m_currency(currency), 
  m_logManager(log),
  logger(log, "simplewallet"),
  m_refresh_progress_reporter(*this), 
  m_initResultPromise(nullptr),
  m_walletSynchronized(false) {
  m_consoleHandler.setHandler("start_mining", boost::bind(&simple_wallet::start_mining, this, _1), "start_mining [<number_of_threads>] - Start mining in daemon");
  m_consoleHandler.setHandler("stop_mining", boost::bind(&simple_wallet::stop_mining, this, _1), "Stop mining in daemon");
  //m_consoleHandler.setHandler("refresh", boost::bind(&simple_wallet::refresh, this, _1), "Resynchronize transactions and balance");
  m_consoleHandler.setHandler("create_integrated", boost::bind(&simple_wallet::create_integrated, this, _1), "create_integrated <payment_id> - Create an integrated address with a payment ID");
  m_consoleHandler.setHandler("export_keys", boost::bind(&simple_wallet::export_keys, this, _1), "Show the secret keys of the openned wallet");
  m_consoleHandler.setHandler("balance", boost::bind(&simple_wallet::show_balance, this, _1), "Show current wallet balance");
  m_consoleHandler.setHandler("incoming_transfers", boost::bind(&simple_wallet::show_incoming_transfers, this, _1), "Show incoming transfers");
  m_consoleHandler.setHandler("outgoing_transfers", boost::bind(&simple_wallet::show_outgoing_transfers, this, _1), "Show outgoing transfers");
  m_consoleHandler.setHandler("list_transfers", boost::bind(&simple_wallet::listTransfers, this, _1), "Show all known transfers");
  m_consoleHandler.setHandler("payments", boost::bind(&simple_wallet::show_payments, this, _1), "payments <payment_id_1> [<payment_id_2> ... <payment_id_N>] - Show payments <payment_id_1>, ... <payment_id_N>");
  m_consoleHandler.setHandler("outputs", boost::bind(&simple_wallet::get_unlocked_outputs, this, _1), "Show unlocked outputs available for a transaction");
  m_consoleHandler.setHandler("bc_height", boost::bind(&simple_wallet::show_blockchain_height, this, _1), "Show blockchain height");
  m_consoleHandler.setHandler("transfer", boost::bind(&simple_wallet::transfer, this, _1),
    "transfer <mixin_count> <addr_1> <amount_1> [<addr_2> <amount_2> ... <addr_N> <amount_N>] [-p payment_id] [-f fee]"
    " - Transfer <amount_1>,... <amount_N> to <address_1>,... <address_N>, respectively. "
    "<mixin_count> is the number of transactions yours is indistinguishable from (from 0 to maximum available)");
  m_consoleHandler.setHandler("log_level", boost::bind(&simple_wallet::set_log, this, _1), "log_level <level> - Change current log level, <level> is a number 0-4");
  m_consoleHandler.setHandler("address", boost::bind(&simple_wallet::print_address, this, _1), "Show current wallet public address");
  m_consoleHandler.setHandler("save", boost::bind(&simple_wallet::save, this, _1), "Save wallet synchronized data");
  m_consoleHandler.setHandler("payment_id", boost::bind(&simple_wallet::payment_id, this, _1), "Generate random Payment ID");
  m_consoleHandler.setHandler("password", boost::bind(&simple_wallet::change_password, this, _1), "Change password");
  m_consoleHandler.setHandler("deposit", boost::bind(&simple_wallet::deposit, this, _1), "deposit <amount> <term> [fee] [mixin] - Deposit amount for duration a duration, term in number of months");
  m_consoleHandler.setHandler("deposit_list", boost::bind(&simple_wallet::deposit_list, this, _1), "deposit_list - Shows list of deposits");
  m_consoleHandler.setHandler("withdraw", boost::bind(&simple_wallet::withdraw, this, _1), "withdraw <index> - Withdraw unlocked deposit");
  m_consoleHandler.setHandler("calculate_interest", boost::bind(&simple_wallet::calculate_interest, this, _1), "calculate_intereset <amount> <term> - Calculate interest for deposit amount for duration, term in number of months");
  m_consoleHandler.setHandler("tx_key", boost::bind(&simple_wallet::get_tx_key, this, _1), "Get secret transaction key for a given <txid>");
  m_consoleHandler.setHandler("tx_proof", boost::bind(&simple_wallet::get_tx_proof, this, _1), "Generate a signature to prove payment: <txid> <address> [<txkey>]");
  m_consoleHandler.setHandler("reserve_proof", boost::bind(&simple_wallet::get_reserve_proof, this, _1), "all|<amount> [<message>] - Generate a signature proving that you own at least <amount>, optionally with a challenge string <message>.\n"
	  "If 'all' is specified, you prove the entire accounts' balance.\n");
  m_consoleHandler.setHandler("reset", boost::bind(&simple_wallet::reset, this, _1), "Discard cache data and start synchronizing from the start");
  m_consoleHandler.setHandler("sign", boost::bind(&simple_wallet::sign_message, this, _1), "Sign the message");
  m_consoleHandler.setHandler("verify", boost::bind(&simple_wallet::verify_message, this, _1), "Verify a signature of the message");
  m_consoleHandler.setHandler("show_seed", boost::bind(&simple_wallet::seed, this, _1), "Get wallet recovery phrase (deterministic seed)");
  m_consoleHandler.setHandler("help", boost::bind(&simple_wallet::help, this, _1), "Show this help");
  m_consoleHandler.setHandler("exit", boost::bind(&simple_wallet::exit, this, _1), "Close wallet");
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_log(const std::vector<std::string> &args) {
  if (args.size() != 1) {
    fail_msg_writer() << "use: set_log <log_level_number_0-4>";
    return true;
  }

  uint16_t l = 0;
  if (!Common::fromString(args[0], l)) {
    fail_msg_writer() << "wrong number format, use: set_log <log_level_number_0-4>";
    return true;
  }
 
  if (l > Logging::TRACE) {
    fail_msg_writer() << "wrong number range, use: set_log <log_level_number_0-4>";
    return true;
  }

  m_logManager.setMaxLevel(static_cast<Logging::Level>(l));
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_tx_key(const std::vector<std::string> &args) {
	if (args.size() != 1)
	{
		fail_msg_writer() << "use: tx_key <txid>";
		return true;
	}
	const std::string &str_hash = args[0];
	Crypto::Hash txid;
	if (!parse_hash256(str_hash, txid)) {
		fail_msg_writer() << "Failed to parse txid";
		return true;
	}

	Crypto::SecretKey tx_key = m_wallet->getTxKey(txid);
	if (tx_key != NULL_SECRET_KEY) {
		success_msg_writer() << "TX KEY # " << Common::podToHex(tx_key);
		return true;
	}
	else {
		fail_msg_writer() << "No tx key found for this txid";
		return true;
	}
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_tx_proof(const std::vector<std::string> &args){
  if(args.size() != 2 && args.size() != 3) {
    fail_msg_writer() << "Usage: tx_proof <txid> <dest_address> [<txkey>]";
    return true;
  }

  const std::string &str_hash = args[0];
  Crypto::Hash txid;
  if (!parse_hash256(str_hash, txid)) {
    fail_msg_writer() << "Failed to parse txid";
    return true;
  }

  const std::string address_string = args[1];
  CryptoNote::AccountPublicAddress address;
  if (!m_currency.parseAccountAddressString(address_string, address)) {
     fail_msg_writer() << "Failed to parse address " << address_string;
     return true;
  }

  std::string sig_str;
  Crypto::SecretKey tx_key, tx_key2;
  bool r = m_wallet->get_tx_key(txid, tx_key);

  if (args.size() == 3) {
    Crypto::Hash tx_key_hash;
    size_t size;
    if (!Common::fromHex(args[2], &tx_key_hash, sizeof(tx_key_hash), size) || size != sizeof(tx_key_hash)) {
      fail_msg_writer() << "failed to parse tx_key";
      return true;
    }
    tx_key2 = *(struct Crypto::SecretKey *) &tx_key_hash;
  
    if (r) {
      if (args.size() == 3 && tx_key != tx_key2) {
        fail_msg_writer() << "Tx secret key was found for the given txid, but you've also provided another tx secret key which doesn't match the found one.";
        return true;
      }
    }
	tx_key = tx_key2;
  } else {
    if (!r) {
      fail_msg_writer() << "Tx secret key wasn't found in the wallet file. Provide it as the optional third parameter if you have it elsewhere.";
      return true;
    }
  }
 
  if (m_wallet->getTxProof(txid, address, tx_key, sig_str)) {
    success_msg_writer() << "Signature: " << sig_str << std::endl;
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::check_tx_proof(const std::vector<std::string> &args) {
  if (args.size() != 3) {
    fail_msg_writer() << "usage: check_tx_proof <txid> <address> <signature>";
	return true;
  }

  // parse txid
  const std::string &str_hash = args[0];
  Crypto::Hash txid;
  if (!parse_hash256(str_hash, txid)) {
    fail_msg_writer() << "Failed to parse txid";
    return true;
  }

  // parse address
  const std::string address_string = args[1];
  CryptoNote::AccountPublicAddress address;
  if (!m_currency.parseAccountAddressString(address_string, address)) {
    fail_msg_writer() << "Failed to parse address " << address_string;
    return true;
  }

  // parse pubkey r*A & signature
  std::string sig_str = args[2];
  if (m_wallet->checkTxProof(txid, address, sig_str)) {
    success_msg_writer() << "Good signature";
  }
  else {
    fail_msg_writer() << "Bad signature";
	return true;
  }

  // TODO: display what's received in tx

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_reserve_proof(const std::vector<std::string> &args){
	if (args.size() != 1 && args.size() != 2) {
		fail_msg_writer() << "Usage: reserve_proof (all|<amount>) [<message>]";
		return true;
	}

	if (m_trackingWallet) {
		fail_msg_writer() << "This is tracking wallet. The reserve proof can be generated only by a full wallet.";
		return true;
	}

	uint64_t reserve = 0;
	if (args[0] != "all") {
		if (!m_currency.parseAmount(args[0], reserve)) {
			fail_msg_writer() << "amount is wrong: " << args[0];
			return true;
		}
	} else {
		reserve = m_wallet->actualBalance();
	}

	try {
		const std::string sig_str = m_wallet->getReserveProof(reserve, args.size() == 2 ? args[1] : "");
		
		//logger(INFO, BRIGHT_WHITE) << "\n\n" << sig_str << "\n\n" << std::endl;

		const std::string filename = "reserve_proof.txt";
		boost::system::error_code ec;
		if (boost::filesystem::exists(filename, ec)) {
			boost::filesystem::remove(filename, ec);
		}

		std::ofstream proofFile(filename, std::ios::out | std::ios::trunc | std::ios::binary);
		if (!proofFile.good()) {
			return false;
		}
		proofFile << sig_str;

		success_msg_writer() << "signature file saved to: " << filename;

	} catch (const std::exception &e) {
		fail_msg_writer() << e.what();
	}

	return true;
}

std::string generatePaymentID(){
  return Common::podToHex(Crypto::rand<Crypto::Hash>());
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::payment_id(const std::vector<std::string> &args) {
  logger(INFO, GREEN) 
    << "Payment ID      # " << generatePaymentID();
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::init(const boost::program_options::variables_map& vm) {
  handle_command_line(vm);

  if (!m_daemon_address.empty() && (!m_daemon_host.empty() || 0 != m_daemon_port)) {
    fail_msg_writer() << "you can't specify daemon host or port several times";
    return false;
  }

  if (m_generate_new.empty() && m_wallet_file_arg.empty()) {
    std::cout << "Nor 'generate-new-wallet' neither 'wallet-file' argument was specified."<<ENDL;
    std::cout << "What do you want to do? "<<ENDL;
    std::cout << "---------------------------------"<<ENDL;
    std::cout << "[G]enerate new wallet   "<<ENDL;
    std::cout << "[O]pen existing wallet  "<<ENDL;
    std::cout << "========> IMPORT WALLET <========"<<ENDL;
    std::cout << "[I]mport with keys (spend/view)  "<<ENDL;
    std::cout << "[M]nemonic seeds                 "<<ENDL;
    std::cout << "[R]restore from private key (GUI)"<<ENDL;
    std::cout << "[T]racking wallet                "<<ENDL;
    std::cout << "[E]xit"<<ENDL;
    std::cout << "---------------------------------"<<ENDL;
    char c;
    do {
        std::string answer;
        std::cout << "##.~> ";
        std::getline(std::cin, answer);
        c = answer[0];
        if (!(c == 'O' || c == 'G' || c == 'E' || c == 'I' || c == 'R' || c == 'T' || c == 'M' || 
              c == 'o' || c == 'g' || c == 'e' || c == 'i' || c == 'r' || c == 't' || c == 'm')) {
            std::cout << "Unknown command: " << c << std::endl;
        } else {
            break;
        }
    } while (true);

    if (c == 'E' || c == 'e') {
        return false;
    }

    std::cout << "Specify wallet file name (e.g., wallet.bin).\n";
    std::string userInput;
    bool validInput = true;

    do {
        std::cout << "WALLET FILE NAME # ";
        std::getline(std::cin, userInput);
        boost::algorithm::trim(userInput);

        if (c != 'o' && c != 'O') {
            std::string ignoredString;
            std::string walletFileName;

            WalletHelper::prepareFileNames(userInput, ignoredString, walletFileName);
            boost::system::error_code ignore;
            if (boost::filesystem::exists(walletFileName, ignore)) {
                std::cout << walletFileName << " already exists! Try a different name." << std::endl;
                validInput = false;
            } else {
                validInput = true;
            }
        }
        if (userInput.empty())
            validInput = false;

    } while (!validInput);

    if (c == 'i' || c == 'I') {
      m_import_new = userInput;
    } else if (c == 'g' || c == 'G') {
        m_generate_new = userInput;
    } else if (c == 'm' || c == 'M') {
        m_mnemonic_new = userInput;
    } else if (c == 'r' || c == 'R') {
      m_restore_new = userInput;
    } else if (c == 't' || c == 'T') {
      m_track_new = userInput;
    }else {
      m_wallet_file_arg = userInput;
    }
  }

  if (!m_generate_new.empty() && !m_wallet_file_arg.empty()) {
      fail_msg_writer() << "You can't specify 'generate-new-wallet' and 'wallet-file' arguments simultaneously";
      return false;
  }

  if (!m_generate_new.empty() && m_restore_deterministic_wallet) {
      fail_msg_writer() << "You can't generate new and restore wallet simultaneously.";
      return false;
  }

  std::string walletFileName;
  if (!m_generate_new.empty() || !m_import_new.empty() || !m_mnemonic_new.empty() || !m_restore_new.empty() || !m_track_new.empty()) {
      std::string ignoredString;
      if (!m_generate_new.empty())
          WalletHelper::prepareFileNames(m_generate_new, ignoredString, walletFileName);
      else if (!m_import_new.empty())
          WalletHelper::prepareFileNames(m_import_new, ignoredString, walletFileName);
      else if (!m_mnemonic_new.empty())
          WalletHelper::prepareFileNames(m_mnemonic_new, ignoredString, walletFileName);
      else if (!m_restore_new.empty())
          WalletHelper::prepareFileNames(m_restore_new, ignoredString, walletFileName);
      else if (!m_track_new.empty())
          WalletHelper::prepareFileNames(m_track_new, ignoredString, walletFileName);

      boost::system::error_code ignore;
      if (boost::filesystem::exists(walletFileName, ignore)) {
          fail_msg_writer() << walletFileName << " already exists";
          return false;
      }
  }

  if (m_daemon_host.empty())
    m_daemon_host = "localhost";
  if (!m_daemon_port)
    m_daemon_port = RPC_DEFAULT_PORT;
  
  if (!m_daemon_address.empty()) {
    if (!parseUrlAddress(m_daemon_address, m_daemon_host, m_daemon_port)) {
      fail_msg_writer() << "failed to parse daemon address: " << m_daemon_address;
      return false;
    }
    remote_fee_address = getFeeAddress();
  } else {
    if (!m_daemon_host.empty())
			remote_fee_address = getFeeAddress();
    m_daemon_address = std::string("http://") + m_daemon_host + ":" + std::to_string(m_daemon_port);
  }

  if (command_line::has_arg(vm, arg_password)) {
    pwd_container.password(command_line::get_arg(vm, arg_password));
  } else if (!pwd_container.read_password()) {
    fail_msg_writer() << "failed to read wallet password";
    return false;
  }

  this->m_node.reset(new NodeRpcProxy(m_daemon_host, m_daemon_port));

  std::promise<std::error_code> errorPromise;
  std::future<std::error_code> f_error = errorPromise.get_future();
  auto callback = [&errorPromise](std::error_code e) {errorPromise.set_value(e); };

  m_node->addObserver(static_cast<INodeRpcProxyObserver*>(this));
  m_node->init(callback);
  auto error = f_error.get();
  if (error) {
    fail_msg_writer() << "failed to init NodeRPCProxy: " << error.message();
    return false;
  }

  if (m_restore_deterministic_wallet && !m_wallet_file_arg.empty()) {
      // check for recover flag. If present, require electrum word list (only recovery option for now).
      if (m_restore_deterministic_wallet) {
          if (m_non_deterministic) {
              fail_msg_writer() << "Cannot specify both --restore-deterministic-wallet and --non-deterministic";
              return false;
          }

          if (m_mnemonic_seed.empty()) {
              std::cout << "MNEMONICS PHRASE (25 WORDS) # ";
              std::getline(std::cin, m_mnemonic_seed);

              if (m_mnemonic_seed.empty()) {
                  fail_msg_writer() << "Specify a recovery parameter with the --mnemonic-seed=\"words list here\"";
                  return false;
              }
          }
          std::string lang = "English";
          if (!Crypto::ElectrumWords::words_to_bytes(m_mnemonic_seed, m_recovery_key, lang)) {
              fail_msg_writer() << "Electrum-style word list failed verification";
              return false;
          }
      }
      std::string               walletAddressFile = prepareWalletAddressFilename(m_wallet_file_arg);
      boost::system::error_code ignore;
      if (boost::filesystem::exists(walletAddressFile, ignore)) {
          logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
          return false;
      }

      bool r = gen_wallet(m_wallet_file_arg, pwd_container.password(), m_recovery_key, m_restore_deterministic_wallet, m_non_deterministic);
      if (!r) {
          logger(ERROR, BRIGHT_RED) << "Account creation failed";
          return false;
      }
  }

  if (!m_generate_new.empty()) {
      std::string walletAddressFile = prepareWalletAddressFilename(m_generate_new);
      boost::system::error_code ignore;
      if (boost::filesystem::exists(walletAddressFile, ignore)) {
          logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
          return false;
      }

      if (!new_wallet(walletFileName, pwd_container.password())) {
          logger(ERROR, BRIGHT_RED) << "account creation failed";
          return false;
      }

      if (!writeAddressFile(walletAddressFile, m_wallet->getAddress())) {
          logger(WARNING, BRIGHT_RED) << "Couldn't write wallet address file: " + walletAddressFile;
      }
  } else if (!m_import_new.empty()) {
      std::string walletAddressFile = prepareWalletAddressFilename(m_import_new);
      boost::system::error_code ignore;
      if (boost::filesystem::exists(walletAddressFile, ignore)) {
          logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
          return false;
      }

      std::string private_spend_key_string;
      std::string private_view_key_string;
      do {
          std::cout << "PRIVATE SPEND KEY# ";
          std::getline(std::cin, private_spend_key_string);
          boost::algorithm::trim(private_spend_key_string);
      } while (private_spend_key_string.empty());
      do {
          std::cout << "PRIVATE VIEW KEY # ";
          std::getline(std::cin, private_view_key_string);
          boost::algorithm::trim(private_view_key_string);
      } while (private_view_key_string.empty());

      Crypto::Hash private_spend_key_hash;
      Crypto::Hash private_view_key_hash;
      size_t size;
      if (!Common::fromHex(private_spend_key_string, &private_spend_key_hash, sizeof(private_spend_key_hash), size) || size != sizeof(private_spend_key_hash)) {
          return false;
      }
      if (!Common::fromHex(private_view_key_string, &private_view_key_hash, sizeof(private_view_key_hash), size) || size != sizeof(private_spend_key_hash)) {
          return false;
      }
      Crypto::SecretKey private_spend_key = *(struct Crypto::SecretKey*)&private_spend_key_hash;
      Crypto::SecretKey private_view_key = *(struct Crypto::SecretKey*)&private_view_key_hash;

      if (!new_wallet(private_spend_key, private_view_key, walletFileName, pwd_container.password())) {
          logger(ERROR, BRIGHT_RED) << "Account creation failed";
          return false;
      }

      if (!writeAddressFile(walletAddressFile, m_wallet->getAddress())) {
          logger(WARNING, BRIGHT_RED) << "Couldn't write wallet address file: " + walletAddressFile;
      }
  } else if (!m_mnemonic_new.empty()) {
      std::string walletAddressFile = prepareWalletAddressFilename(m_mnemonic_new);
      boost::system::error_code ignore;
      if (boost::filesystem::exists(walletAddressFile, ignore)) {
          logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
          return false;
      }

      do {
          std::cout << "MNEMONICS PHRASE (25 WORDS) # ";
          std::getline(std::cin, m_mnemonic_seed);

      } while (m_mnemonic_seed.empty());
      std::string lang = "English";
      if (!Crypto::ElectrumWords::words_to_bytes(m_mnemonic_seed, m_recovery_key, lang)) {
          fail_msg_writer() << "Electrum-style word list failed verification";
          return false;
      }

      std::cout << Common::podToHex(m_recovery_key) << " ==== " << ENDL;
      std::cout << m_mnemonic_new << " ==== " << ENDL;
      bool r = gen_wallet(walletFileName, pwd_container.password(), m_recovery_key, true, false);
      if (!r) {
          logger(ERROR, BRIGHT_RED) << "Account creation failed";
          return false;
      }
      
      if (!writeAddressFile(walletAddressFile, m_wallet->getAddress())) {
          logger(WARNING, BRIGHT_RED) << "Couldn't write wallet address file: " + walletAddressFile;
      }
  } else if (!m_restore_new.empty()) {
      std::string walletAddressFile = prepareWalletAddressFilename(m_restore_new);
      boost::system::error_code ignore;
      if (boost::filesystem::exists(walletAddressFile, ignore)) {
          logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
          return false;
      }

      std::string private_key_string;

      do {
          std::cout << "PRIVATE KEY      [GUI] # ";
          std::getline(std::cin, private_key_string);
          boost::algorithm::trim(private_key_string);
      } while (private_key_string.empty());

      std::string data;

      if (private_key_string.length() != 256) {
          logger(ERROR, BRIGHT_RED) << "Wrong Private key.";
          return false;
      }

      std::string public_spend_key_string = private_key_string.substr(0, 64);
      std::string public_view_key_string = private_key_string.substr(64, 64);
      std::string private_spend_key_string = private_key_string.substr(128, 64);
      std::string private_view_key_string = private_key_string.substr(192, 64);

      Crypto::Hash public_spend_key_hash;
      Crypto::Hash public_view_key_hash;
      Crypto::Hash private_spend_key_hash;
      Crypto::Hash private_view_key_hash;

      size_t size;
      if (!Common::fromHex(public_spend_key_string, &public_spend_key_hash, sizeof(public_spend_key_hash), size) || size != sizeof(public_spend_key_hash))
          return false;
      if (!Common::fromHex(public_view_key_string, &public_view_key_hash, sizeof(public_view_key_hash), size) || size != sizeof(public_view_key_hash))
          return false;
      if (!Common::fromHex(private_spend_key_string, &private_spend_key_hash, sizeof(private_spend_key_hash), size) || size != sizeof(private_spend_key_hash))
          return false;
      if (!Common::fromHex(private_view_key_string, &private_view_key_hash, sizeof(private_view_key_hash), size) || size != sizeof(private_view_key_hash))
          return false;

      Crypto::SecretKey private_spend_key = *(struct Crypto::SecretKey*)&private_spend_key_hash;
      Crypto::SecretKey private_view_key = *(struct Crypto::SecretKey*)&private_view_key_hash;

      if (!new_wallet(private_spend_key, private_view_key, walletFileName, pwd_container.password())) {
          logger(ERROR, BRIGHT_RED) << "Account creation failed";
          return false;
      }

      if (!writeAddressFile(walletAddressFile, m_wallet->getAddress())) {
          logger(WARNING, BRIGHT_RED) << "Couldn't write wallet address file: " + walletAddressFile;
      }
  } else if (!m_track_new.empty()) {
      std::string walletAddressFile = prepareWalletAddressFilename(m_track_new);
      boost::system::error_code ignore;
      if (boost::filesystem::exists(walletAddressFile, ignore)) {
          logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
          return false;
      }

      std::string tracking_key_string;

      do {
          std::cout << "TRACKING KEY     [GUI] #";
          std::getline(std::cin, tracking_key_string);
          boost::algorithm::trim(tracking_key_string);
      } while (tracking_key_string.empty());

      if (tracking_key_string.length() != 256) {
          logger(ERROR, BRIGHT_RED) << "Wrong Tracking key.";
          return false;
      }

      AccountKeys keys;

      std::string public_spend_key_string = tracking_key_string.substr(0, 64);
      std::string public_view_key_string = tracking_key_string.substr(64, 64);
      std::string private_spend_key_string = tracking_key_string.substr(128, 64);
      std::string private_view_key_string = tracking_key_string.substr(192, 64);

      Crypto::Hash public_spend_key_hash;
      Crypto::Hash public_view_key_hash;
      Crypto::Hash private_spend_key_hash;
      Crypto::Hash private_view_key_hash;

      size_t size;
      if (!Common::fromHex(public_spend_key_string, &public_spend_key_hash, sizeof(public_spend_key_hash), size) || size != sizeof(public_spend_key_hash))
          return false;
      if (!Common::fromHex(public_view_key_string, &public_view_key_hash, sizeof(public_view_key_hash), size) || size != sizeof(public_view_key_hash))
          return false;
      if (!Common::fromHex(private_spend_key_string, &private_spend_key_hash, sizeof(private_spend_key_hash), size) || size != sizeof(private_spend_key_hash))
          return false;
      if (!Common::fromHex(private_view_key_string, &private_view_key_hash, sizeof(private_view_key_hash), size) || size != sizeof(private_view_key_hash))
          return false;

      Crypto::PublicKey public_spend_key = *(struct Crypto::PublicKey*)&public_spend_key_hash;
      Crypto::PublicKey public_view_key = *(struct Crypto::PublicKey*)&public_view_key_hash;
      Crypto::SecretKey private_spend_key = *(struct Crypto::SecretKey*)&private_spend_key_hash;
      Crypto::SecretKey private_view_key = *(struct Crypto::SecretKey*)&private_view_key_hash;

      keys.address.spendPublicKey = public_spend_key;
      keys.address.viewPublicKey = public_view_key;
      keys.spendSecretKey = private_spend_key;
      keys.viewSecretKey = private_view_key;

      if (!new_tracking_wallet(keys, walletFileName, pwd_container.password())) {
          logger(ERROR, BRIGHT_RED) << "account creation failed";
          return false;
      }

      if (!writeAddressFile(walletAddressFile, m_wallet->getAddress())) {
          logger(WARNING, BRIGHT_RED) << "Couldn't write wallet address file: " + walletAddressFile;
      }
	} else {
      m_wallet.reset(new WalletLegacy(m_currency, *m_node, m_logManager));

      try {
          m_wallet_file = tryToOpenWalletOrLoadKeysOrThrow(logger, m_wallet, m_wallet_file_arg, pwd_container.password());
      } catch (const std::exception& e) {
          fail_msg_writer() << "failed to load wallet: " << e.what();
          return false;
      }

      m_wallet->addObserver(this);
      m_node->addObserver(static_cast<INodeObserver*>(this));

      logger(INFO, BRIGHT_WHITE) << "Opened wallet: " << m_wallet->getAddress();

      success_msg_writer() << "**********************************************************************\n"
                           << "Use \"help\" command to see the list of available commands.\n"
                           << "**********************************************************************";
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::deinit() {
  m_wallet->removeObserver(this);
  m_node->removeObserver(static_cast<INodeObserver*>(this));
  m_node->removeObserver(static_cast<INodeRpcProxyObserver*>(this));

  if (!m_wallet.get())
    return true;

  return close_wallet();
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::handle_command_line(const boost::program_options::variables_map& vm) {
  m_wallet_file_arg = command_line::get_arg(vm, arg_wallet_file);
  m_generate_new = command_line::get_arg(vm, arg_generate_new_wallet);
  m_daemon_address = command_line::get_arg(vm, arg_daemon_address);
  m_daemon_host = command_line::get_arg(vm, arg_daemon_host);
  m_daemon_port = command_line::get_arg(vm, arg_daemon_port);
  m_restore_deterministic_wallet = command_line::get_arg(vm, arg_restore_deterministic_wallet);
  m_non_deterministic = command_line::get_arg(vm, arg_non_deterministic);
  m_mnemonic_seed = command_line::get_arg(vm, arg_mnemonic_seed);
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::gen_wallet(const std::string &wallet_file, const std::string& password, const Crypto::SecretKey& recovery_key, bool recover, bool two_random) {
  m_wallet_file = wallet_file;

  m_wallet.reset(new WalletLegacy(m_currency, *m_node.get(), m_logManager));
  m_node->addObserver(static_cast<INodeObserver*>(this));
  m_wallet->addObserver(this);

  Crypto::SecretKey recovery_val;
  try {
      m_initResultPromise.reset(new std::promise<std::error_code>());
      std::future<std::error_code> f_initError = m_initResultPromise->get_future();

      recovery_val = m_wallet->generateKey(password, recovery_key, recover, two_random);
      auto initError = f_initError.get();
      m_initResultPromise.reset(nullptr);
      if (initError) {
          fail_msg_writer() << "Failed to generate new wallet # " << initError.message();
          return false;
      }

      try {
          CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
      } catch (std::exception& e) {
          fail_msg_writer() << "Failed to save new wallet     # " << e.what();
          throw;
      }

      AccountKeys keys;
      m_wallet->getAccountKeys(keys);

      logger(INFO, BRIGHT_WHITE) << "NEW WALLET # " << m_wallet->getAddress();
      logger(INFO, BRIGHT_WHITE) << "VIEW KEY   # " << Common::podToHex(keys.viewSecretKey);
  } catch (const std::exception& e) {
      fail_msg_writer() << "failed to generate new wallet: " << e.what();
      return false;
  }

  // convert rng value to electrum-style word list
  std::string lang = "English";
  std::string electrum_words;
  Crypto::ElectrumWords::bytes_to_words(recovery_val, electrum_words, lang);
  std::string print_electrum = "";

  success_msg_writer() << "**********************************************************************\n"
                       << "Your wallet has been generated.\n"
                       << "Use \"help\" command to see the list of available commands.\n"
                       << "Always use \"exit\" command when closing simplewallet to save\n"
                       << "current session's state. Otherwise, you will possibly need to synchronize \n"
                       << "your wallet again. Your wallet key is NOT under risk anyway.\n";
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::new_wallet(const std::string &wallet_file, const std::string& password) {
  m_wallet_file = wallet_file;

  m_wallet.reset(new WalletLegacy(m_currency, *m_node.get(), m_logManager));
  m_node->addObserver(static_cast<INodeObserver*>(this));
  m_wallet->addObserver(this);
  try {
    m_initResultPromise.reset(new std::promise<std::error_code>());
    std::future<std::error_code> f_initError = m_initResultPromise->get_future();
    m_wallet->initAndGenerateDeterministic(password);
    auto initError = f_initError.get();
    m_initResultPromise.reset(nullptr);
    if (initError) {
      fail_msg_writer() << "failed to generate new wallet: " << initError.message();
      return false;
    }

    try {
      CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    } catch (std::exception& e) {
      fail_msg_writer() << "failed to save new wallet: " << e.what();
      throw;
    }

    AccountKeys keys;
    m_wallet->getAccountKeys(keys);

    logger(INFO, BRIGHT_WHITE) <<
      "GENERATED NEW WALLET # " << m_wallet->getAddress() << std::endl <<
      "VIEW KEY             # " << Common::podToHex(keys.viewSecretKey);
  }
  catch (const std::exception& e) {
    fail_msg_writer() << "failed to generate new wallet: " << e.what();
    return false;
  }

  AccountKeys keys;
  m_wallet->getAccountKeys(keys);
  // convert rng value to electrum-style word list
  std::string lang = "English";
  std::string electrum_words;
  Crypto::ElectrumWords::bytes_to_words(keys.spendSecretKey, electrum_words, lang);
  std::string print_electrum = "";

  success_msg_writer() <<
    "**********************************************************************\n" <<
    "Your wallet has been generated.\n" <<
    "Use \"help\" command to see the list of available commands.\n" <<
    "Always use \"exit\" command when closing simplewallet to save\n" <<
    "current session's state. Otherwise, you will possibly need to synchronize \n" <<
    "your wallet again. Your wallet key is NOT under risk anyway.\n" <<
    "**********************************************************************";

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::new_wallet(Crypto::SecretKey& secret_key, Crypto::SecretKey& view_key, const std::string& wallet_file, const std::string& password) {
    m_wallet_file = wallet_file;

    m_wallet.reset(new WalletLegacy(m_currency, *m_node.get(), m_logManager));
    m_node->addObserver(static_cast<INodeObserver*>(this));
    m_wallet->addObserver(this);
    try {
        m_initResultPromise.reset(new std::promise<std::error_code>());
        std::future<std::error_code> f_initError = m_initResultPromise->get_future();

        AccountKeys wallet_keys;
        wallet_keys.spendSecretKey = secret_key;
        wallet_keys.viewSecretKey = view_key;
        Crypto::secret_key_to_public_key(wallet_keys.spendSecretKey, wallet_keys.address.spendPublicKey);
        Crypto::secret_key_to_public_key(wallet_keys.viewSecretKey, wallet_keys.address.viewPublicKey);

        m_wallet->initWithKeys(wallet_keys, password);
        auto initError = f_initError.get();
        m_initResultPromise.reset(nullptr);
        if (initError) {
            fail_msg_writer() << "failed to generate new wallet: " << initError.message();
            return false;
        }

        try {
            CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
        } catch (std::exception& e) {
            fail_msg_writer() << "failed to save new wallet: " << e.what();
            throw;
        }

        AccountKeys keys;
        m_wallet->getAccountKeys(keys);

        logger(INFO, BRIGHT_WHITE) << "IMPORTED WALLET        # " << m_wallet->getAddress() << std::endl;
    } catch (const std::exception& e) {
        fail_msg_writer() << "failed to import wallet: " << e.what();
        return false;
    }

    logger(INFO, BRIGHT_GREEN)  << "**********************************************************************";
    logger(INFO, BRIGHT_GREEN)  << "Your wallet has been imported." ;
    logger(INFO, BRIGHT_GREEN)  << "Use \"help\" command to see the list of available commands." ;
    logger(INFO, BRIGHT_GREEN)  << "Always use \"exit\" command when closing simplewallet to save" ;
    logger(INFO, BRIGHT_GREEN)  << "current session's state. Otherwise, you will possibly need to synchronize" ;
    logger(INFO, BRIGHT_GREEN)  << "your wallet again. Your wallet key is NOT under risk anyway." ;
    logger(INFO, BRIGHT_GREEN)  << "**********************************************************************" ;
    return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::new_wallet(AccountKeys &private_key, const std::string &wallet_file, const std::string& password) {
    m_wallet_file = wallet_file;

    m_wallet.reset(new WalletLegacy(m_currency, *m_node.get(), m_logManager));
    m_node->addObserver(static_cast<INodeObserver*>(this));
    m_wallet->addObserver(this);
    try {
        m_initResultPromise.reset(new std::promise<std::error_code>());
        std::future<std::error_code> f_initError = m_initResultPromise->get_future();

        m_wallet->initWithKeys(private_key, password);
        auto initError = f_initError.get();
        m_initResultPromise.reset(nullptr);
        if (initError) {
            fail_msg_writer() << "failed to generate new wallet: " << initError.message();
            return false;
        }

        try {
            CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
        }
        catch (std::exception& e) {
            fail_msg_writer() << "failed to save new wallet: " << e.what();
            throw;
        }

        AccountKeys keys;
        m_wallet->getAccountKeys(keys);

        logger(INFO, BRIGHT_WHITE) <<
            "Imported wallet: " << m_wallet->getAddress() << std::endl;

        if (keys.spendSecretKey == boost::value_initialized<Crypto::SecretKey>()) {
           m_trackingWallet = true;
        }
    }
    catch (const std::exception& e) {
        fail_msg_writer() << "failed to import wallet: " << e.what();
        return false;
    }

    success_msg_writer() <<
        "**********************************************************************\n" <<
        "Your wallet has been imported.\n" <<
        "Use \"help\" command to see the list of available commands.\n" <<
        "Always use \"exit\" command when closing simplewallet to save\n" <<
        "current session's state. Otherwise, you will possibly need to synchronize \n" <<
        "your wallet again. Your wallet key is NOT under risk anyway.\n" <<
        "**********************************************************************";
    return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::new_tracking_wallet(AccountKeys &tracking_key, const std::string &wallet_file, const std::string& password) {
    m_wallet_file = wallet_file;

    m_wallet.reset(new WalletLegacy(m_currency, *m_node.get(), m_logManager));
    m_node->addObserver(static_cast<INodeObserver*>(this));
    m_wallet->addObserver(this);
    try {
        m_initResultPromise.reset(new std::promise<std::error_code>());
        std::future<std::error_code> f_initError = m_initResultPromise->get_future();

        m_wallet->initWithKeys(tracking_key, password);
        auto initError = f_initError.get();
        m_initResultPromise.reset(nullptr);
        if (initError) {
            fail_msg_writer() << "failed to generate new wallet: " << initError.message();
            return false;
        }

        try {
            CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
        }
        catch (std::exception& e) {
            fail_msg_writer() << "failed to save new wallet: " << e.what();
            throw;
        }

        AccountKeys keys;
        m_wallet->getAccountKeys(keys);

        logger(INFO, BRIGHT_WHITE) <<
            "TRACKING WALLET        # " << m_wallet->getAddress() << std::endl;

        m_trackingWallet = true;
    }
    catch (const std::exception& e) {
        fail_msg_writer() << "failed to import wallet: " << e.what();
        return false;
    }

    logger(INFO, BRIGHT_GREEN) << "**********************************************************************" ;
    logger(INFO, BRIGHT_GREEN) << "Your tracking wallet has been imported. It doesn't allow spending funds.";
    logger(INFO, BRIGHT_GREEN) << "It allows to view incoming transactions but not outgoing ones." ;
    logger(INFO, BRIGHT_GREEN) << "If there were spendings total balance will be inaccurate." ;
    logger(INFO, BRIGHT_GREEN) << "Use \"help\" command to see the list of available commands." ;
    logger(INFO, BRIGHT_GREEN) << "Always use \"exit\" command when closing simplewallet to save\n" ;
    logger(INFO, BRIGHT_GREEN) << "current session's state. Otherwise, you will possibly need to synchronize " ;
    logger(INFO, BRIGHT_GREEN) << "your wallet again. Your wallet key is NOT under risk anyway." ;
    logger(INFO, BRIGHT_GREEN) << "**********************************************************************";
    return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::close_wallet(){
  try {
    CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
  } catch (const std::exception& e) {
    fail_msg_writer() << e.what();
    return false;
  }

  m_wallet->removeObserver(this);
  m_wallet->shutdown();

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::save(const std::vector<std::string> &args){
  try {
    CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    success_msg_writer() << "Wallet data saved";
  } catch (const std::exception& e) {
    fail_msg_writer() << e.what();
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::reset(const std::vector<std::string> &args) {

  AccountKeys keys;
  m_wallet->getAccountKeys(keys);
  std::string electrum_words;

  Crypto::secret_key_to_public_key(keys.spendSecretKey, keys.address.spendPublicKey);
  Crypto::secret_key_to_public_key(keys.viewSecretKey, keys.address.viewPublicKey);

  boost::system::error_code renameEc;
  boost::filesystem::rename(m_wallet_file, m_wallet_file + ".backup", renameEc);
  if (renameEc) {
    throw std::runtime_error("failed to rename file '" + m_wallet_file + "' to '" + m_wallet_file + ".backup" + "': " + renameEc.message());
  }

  if (!new_wallet(keys.spendSecretKey, keys.viewSecretKey, m_wallet_file, pwd_container.password())) {
      logger(ERROR, BRIGHT_RED) << "Account reset failed";
      return false;
  }

  return true;

  // {
  //   std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
  //   m_walletSynchronized = false;
  // }

  // m_wallet->reset();
  // success_msg_writer(true) << "Reset completed successfully.";

  // std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
  // while (!m_walletSynchronized) {
  //   m_walletSynchronizedCV.wait(lock);
  // }

  // std::cout << std::endl;

  // return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::change_password(const std::vector<std::string>& args) {
  std::cout << "....OLD ";
  m_consoleHandler.pause();
  if (!pwd_container.read_and_validate()) {
    std::cout << "Incorrect password!" << std::endl;
    m_consoleHandler.unpause();
    return false;
  }
  const auto oldpwd = pwd_container.password();

  std::cout << "....NEW ";
  pwd_container.read_password(true);
  const auto newpwd = pwd_container.password();
  m_consoleHandler.unpause();

  try
	{
		m_wallet->changePassword(oldpwd, newpwd);
	}
	catch (const std::exception& e) {
		fail_msg_writer() << "Could not change password: " << e.what();
		return false;
	}
	success_msg_writer(true) << "Password changed.";
	return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::deposit(const std::vector<std::string>& args) {
  if (args.empty() || (args.size() < 2 && args.size() > 4)) {
      fail_msg_writer() << "usage: deposit <amount> <term> [fee] [mixin]- Term in number of months";
      return true;
  }

  uint64_t term   = 0;
  uint64_t amount = 0;
  uint64_t fee    = CryptoNote::parameters::MINIMUM_FEE;
  uint64_t mixin  = 0;
  m_currency.parseAmount(args[0], amount);
  std::stringstream ss;
  ss << args[1];
  ss >> term;
  ss.clear();
  
  if ( args.size() == 3 ){
    fee = m_currency.parseAmount(args[2], fee);
    if( fee < CryptoNote::parameters::MINIMUM_FEE ){
      logger(ERROR, RED) << "Insufficient fee. " << ENDL;
      return false;
    }
  }
  if ( args.size() == 4 ){
    ss << args[3];
    ss >> mixin;
    if( mixin < CryptoNote::parameters::MIN_TX_MIXIN_SIZE || mixin > CryptoNote::parameters::MAX_TX_MIXIN_SIZE ){
      logger(ERROR, RED) << "Invalid mixin size, Please specify between " << CryptoNote::parameters::MIN_TX_MIXIN_SIZE  << " and " << CryptoNote::parameters::MAX_TX_MIXIN_SIZE << ENDL;
      return false;
    }
  }

  if (amount < CryptoNote::parameters::DEPOSIT_MIN_AMOUNT) {
    logger(ERROR, RED) << "Invalid Depoist amount, Minimum deposit amount is " << m_currency.formatAmount(CryptoNote::parameters::DEPOSIT_MIN_AMOUNT) << ENDL;
    return false;
  }

  if( term < 1 && term > 12 ) {
    logger(ERROR, RED) << "Term should be in months, allowed term is between 1 Month to 12 Months";
    return false;
  }

  if (term < CryptoNote::parameters::DEPOSIT_MIN_TERM / CryptoNote::parameters::NUMBER_OF_BLOCKS_PER_DAY || term > CryptoNote::parameters::DEPOSIT_MAX_TERM / CryptoNote::parameters::NUMBER_OF_BLOCKS_PER_DAY) {
    logger(ERROR, RED) << "Invalid Deposit term, Minimum term is 1 month Maximum is 12 months" << ENDL;
    return false;
  }

  if (amount > (m_wallet->actualBalance() + fee) ){
    logger(ERROR, RED) << m_currency.formatAmount(amount) << " Insufficient funds; " << m_currency.formatAmount(m_wallet->actualBalance()) << ENDL;
    return false;
  }

  uint64_t interest = m_currency.calculateInterest(amount, term * CryptoNote::parameters::DEPOSIT_MIN_TERM);

  try {
    CryptoNote::WalletHelper::SendCompleteResultObserver sent;
    WalletHelper::IWalletRemoveObserverGuard removeGuard(*m_wallet, sent);
    CryptoNote::TransactionId tx = m_wallet->deposit(term * CryptoNote::parameters::DEPOSIT_MIN_TERM, amount, fee , 0);
    
    std::error_code sendError = sent.wait(tx);
    removeGuard.removeObserver();

    if (sendError) {
      fail_msg_writer() << sendError.message();
      return true;
    }

    CryptoNote::WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(tx, txInfo);

    success_msg_writer(true) << "Depositing " << m_currency.formatAmount(amount) << " " << CRYPTONOTE_TICKER << " for " << term << " months.";                    
    success_msg_writer(true) << "Deposit meturity amount # " << m_currency.formatAmount(amount + interest) << ENDL ;
    success_msg_writer(true) << "Transaction hash       # " << Common::podToHex(txInfo.hash);
    success_msg_writer(true) << "Transaction secret key # " << Common::podToHex(txInfo.secretKey); 
      try {
        CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
      } catch (const std::exception& e) {
        fail_msg_writer() << e.what();
        return true;
      }
  } catch (const std::system_error& e) {
    fail_msg_writer() << e.what();
  } catch (const std::exception& e) {
    fail_msg_writer() << e.what();
  } catch (...) {
    fail_msg_writer() << "unknown error";
  }
  // } catch (std::system_error& e) {
  //     // unlock();
  //     std::cout<<"Error :: "<< e.;
  // }
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::deposit_list(const std::vector<std::string>& args) {
  bool hasTransfers = false;
  // logger(INFO) << "        AMOUNT       \t                              TX ID";

  std::string header = makeCenteredString(TIMESTAMP_MAX_WIDTH, "CREATE TIME (UTC)") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "INDEX") + "  ";
  header += makeCenteredString(TOTAL_AMOUNT_MAX_WIDTH, "AMOUNT") + "  ";
  header += makeCenteredString(TOTAL_AMOUNT_MAX_WIDTH, "INTEREST") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "TERM") + "  ";
  header += makeCenteredString(12, "STATE") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "CREATED") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "UNLOCKS") + "  ";

  logger(INFO) << std::string(header.size(), '-');
  logger(INFO, GREEN) 
    << std::setw(TIMESTAMP_MAX_WIDTH + TOTAL_AMOUNT_MAX_WIDTH + (4 * BLOCK_MAX_WIDTH) + 14 + 10) << "TOTAL UNLOCKED #"
    << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(m_wallet->actualDepositBalance());
  logger(INFO, GREEN) 
    << std::setw(TIMESTAMP_MAX_WIDTH + TOTAL_AMOUNT_MAX_WIDTH + (4 * BLOCK_MAX_WIDTH) + 14 + 10) << "TOTAL PENDING  #"
    << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(m_wallet->pendingDepositBalance());

  logger(INFO) << std::string(header.size(), '-');
  logger(INFO) << header;
  logger(INFO) << std::string(header.size(), '-');
  
  size_t transactionsCount = m_wallet->getTransactionCount();
  for (size_t transactionNumber = 0; transactionNumber < transactionsCount; ++transactionNumber) {
      WalletLegacyTransaction txInfo;
      m_wallet->getTransaction(transactionNumber, txInfo);
      if (txInfo.totalAmount > 0) continue;
      hasTransfers = true;

      time_t timestamp = static_cast<time_t>(txInfo.timestamp);
      char timeString[TIMESTAMP_MAX_WIDTH + 1];
      if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", std::gmtime(&timestamp)) == 0) {
          throw std::runtime_error("time buffer is too small");
      }
      
      std::string timeStr (timeString);
      std::string heightStr = std::to_string(txInfo.blockHeight);
      if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
        timeStr = "....-..-.. ..:..:..";
        heightStr = "UNCNFM";
      }

      std::string txHash;
      if (static_cast<int64_t>(txInfo.depositCount) > 0) {
          CryptoNote::Deposit deposit;
          m_wallet->getDeposit(txInfo.firstDepositId, deposit);

          std::string state;
          std::string rowColor;
          if (deposit.locked) {
              state = "LOCKED";
              rowColor = YELLOW;
          } else if (deposit.spendingTransactionId == CryptoNote::WALLET_LEGACY_INVALID_TRANSACTION_ID) {
              state = "UNLOCKED";
              rowColor = GREEN;
          } else {
              state = "SPENT";
              rowColor = RED;
          }
          std::string heightStr = std::to_string(txInfo.blockHeight);
          std::string unlockStr = std::to_string(txInfo.blockHeight + deposit.term);
          if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
              timeStr = "....-..-.. ..:..:..";
              heightStr = "UNCNFM";
              unlockStr = "";
          } 

          logger(INFO, rowColor) 
            << std::setw(TIMESTAMP_MAX_WIDTH) << timeStr
            << "  " << std::setw(BLOCK_MAX_WIDTH) << txInfo.firstDepositId
            << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(deposit.amount) 
            << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(deposit.interest)
            << "  " << std::setw(BLOCK_MAX_WIDTH)  << std::right << deposit.term 
            << "  " << std::setw(12) << std::right << state 
            << "  " << std::setw(BLOCK_MAX_WIDTH) << std::right << heightStr 
            << "  " << std::setw(BLOCK_MAX_WIDTH) << std::right << unlockStr;  
            // << "  " << std::setw(5) << txInfo.depositCount << "  " << txInfo.firstDepositId << "   " << Common::podToHex(txInfo.secretKey);
            // std::cout << deposit.spendingTransactionId << " " << deposit.creatingTransactionId << ENDL;
      } 
    }
  logger(INFO) << std::string(header.size(), '-');
  if (!hasTransfers) success_msg_writer() << "No outgoing transfers";
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::withdraw(const std::vector<std::string>& args) {
    std::vector<DepositId> _depositIds;
    try {
      size_t transactionsCount = m_wallet->getTransactionCount();
      for (size_t transactionNumber = 0; transactionNumber < transactionsCount; ++transactionNumber) {
        WalletLegacyTransaction txInfo;
        m_wallet->getTransaction(transactionNumber, txInfo);
        if (txInfo.totalAmount > 0) continue;

        time_t timestamp = static_cast<time_t>(txInfo.timestamp);
        char timeString[TIMESTAMP_MAX_WIDTH + 1];
        if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", std::gmtime(&timestamp)) == 0) {
            throw std::runtime_error("time buffer is too small");
        }

        std::string txHash;
        if (static_cast<int64_t>(txInfo.depositCount) > 0) {
            CryptoNote::Deposit deposit;
            m_wallet->getDeposit(txInfo.firstDepositId, deposit);

            if (deposit.spendingTransactionId == CryptoNote::WALLET_LEGACY_INVALID_TRANSACTION_ID) {
              std::string state = "UNLOCKED";
              std::string rowColor = GREEN;
              std::string unlockStr = std::to_string(txInfo.blockHeight + deposit.term);
              std::string timeStr (timeString);
              std::string heightStr = std::to_string(txInfo.blockHeight);
              _depositIds.push_back(txInfo.firstDepositId);
              logger(INFO, rowColor) 
                << std::setw(TIMESTAMP_MAX_WIDTH) << timeStr
                << "  " << std::setw(BLOCK_MAX_WIDTH) << txInfo.firstDepositId
                << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(deposit.amount) 
                << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(deposit.interest)
                << "  " << std::setw(BLOCK_MAX_WIDTH)  << std::right << deposit.term 
                << "  " << std::setw(12) << std::right << state 
                << "  " << std::setw(BLOCK_MAX_WIDTH) << std::right << heightStr 
                << "  " << std::setw(BLOCK_MAX_WIDTH) << std::right << unlockStr; 
          } 
        } 
      }

      bool confirm = false;
      if( _depositIds.size() > 0 ){
        std::string answer;
        char ans = 'N';
        logger(INFO, RED) << m_currency.formatAmount(m_wallet->actualDepositBalance())  << " will be withdrawn, Are you sure, would you like to proceed.. ? y/N # ";
        std::getline(std::cin, answer);
        ans = answer[0];

        if ( ans == 'y' || ans == 'Y') {
          confirm = true;
        }
      } else {
        logger ( ERROR, RED ) << "No unlocked deposits found." << ENDL;
      }
      if (confirm) {
        CryptoNote::WalletHelper::SendCompleteResultObserver sent;
        WalletHelper::IWalletRemoveObserverGuard removeGuard(*m_wallet, sent);

        CryptoNote::TransactionId tx = m_wallet->withdrawDeposits(_depositIds, CryptoNote::parameters::MINIMUM_FEE);

        std::error_code sendError = sent.wait(tx);
        removeGuard.removeObserver();

        if (sendError) {
          fail_msg_writer() << sendError.message();
          return true;
        }

        CryptoNote::WalletLegacyTransaction txInfo;
        m_wallet->getTransaction(tx, txInfo);

        success_msg_writer(true) << "Money successfully sent.";
        success_msg_writer(true) << "Transaction hash       # " << Common::podToHex(txInfo.hash);
        success_msg_writer(true) << "Transaction secret key # " << Common::podToHex(txInfo.secretKey); 
        try {
          CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
        } catch (const std::exception& e) {
          fail_msg_writer() << e.what();
          return true;
        }
      }
  } catch (const std::system_error& e) {
    fail_msg_writer() << e.what();
  } catch (const std::exception& e) {
    fail_msg_writer() << e.what();
  } catch (...) {
    fail_msg_writer() << "unknown error";
  }

    return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::calculate_interest(const std::vector<std::string>& args) {
  if (args.size() != 2) {
    fail_msg_writer() << "usage: calculate_interest <amount> <term> - Term in number of months";
	return true;
  }

  uint64_t term = 0;
  uint64_t amount = 0;
  m_currency.parseAmount(args[0], amount);
  std::stringstream sterm;
  sterm << args[1];
  sterm >> term;

  if (amount < CryptoNote::parameters::DEPOSIT_MIN_AMOUNT) {
      std::cout << "Minimum amount is " << m_currency.formatAmount(CryptoNote::parameters::DEPOSIT_MIN_AMOUNT) << ENDL;
      return false;
  }
  if (term < 1 || term > 12) {
      std::cout << "Minimum term is 1 month Maximum is 12 months" << ENDL;
      return false;
  }

  uint64_t interest = m_currency.calculateInterest(amount, term * CryptoNote::parameters::DEPOSIT_MIN_TERM);
  std::cout << "Amount  :: " << m_currency.formatAmount(amount) << ENDL
            << "Term    :: " << term << ENDL
            << "Interest:: " << m_currency.formatAmount(interest) << ENDL;
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::start_mining(const std::vector<std::string>& args) {
  COMMAND_RPC_START_MINING::request req;
  req.miner_address = m_wallet->getAddress();

  bool ok = true;
  size_t max_mining_threads_count = (std::max)(std::thread::hardware_concurrency(), static_cast<unsigned>(2));
  if (0 == args.size()) {
    req.threads_count = 1;
  } else if (1 == args.size()) {
    uint16_t num = 1;
    ok = Common::fromString(args[0], num);
    ok = ok && (1 <= num && num <= max_mining_threads_count);
    req.threads_count = num;
  } else {
    ok = false;
  }

  if (!ok) {
    fail_msg_writer() << "invalid arguments. Please use start_mining [<number_of_threads>], " <<
      "<number_of_threads> should be from 1 to " << max_mining_threads_count;
    return true;
  }


  COMMAND_RPC_START_MINING::response res;

  try {
    HttpClient httpClient(m_dispatcher, m_daemon_host, m_daemon_port);

    invokeJsonCommand(httpClient, "/start_mining", req, res);

    std::string err = interpret_rpc_response(true, res.status);
    if (err.empty())
      success_msg_writer() << "Mining started in daemon";
    else
      fail_msg_writer() << "mining has NOT been started: " << err;

  } catch (const ConnectException&) {
    printConnectionError();
  } catch (const std::exception& e) {
    fail_msg_writer() << "Failed to invoke rpc method: " << e.what();
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::stop_mining(const std::vector<std::string>& args){
  COMMAND_RPC_STOP_MINING::request req;
  COMMAND_RPC_STOP_MINING::response res;

  try {
    HttpClient httpClient(m_dispatcher, m_daemon_host, m_daemon_port);

    invokeJsonCommand(httpClient, "/stop_mining", req, res);
    std::string err = interpret_rpc_response(true, res.status);
    if (err.empty())
      success_msg_writer() << "Mining stopped in daemon";
    else
      fail_msg_writer() << "mining has NOT been stopped: " << err;
  } catch (const ConnectException&) {
    printConnectionError();
  } catch (const std::exception& e) {
    fail_msg_writer() << "Failed to invoke rpc method: " << e.what();
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::initCompleted(std::error_code result) {
  if (m_initResultPromise.get() != nullptr) {
    m_initResultPromise->set_value(result);
  }
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::connectionStatusUpdated(bool connected) {
    if (connected) {
        logger(INFO, GREEN) << "Wallet connected to daemon.";
    } else {
        printConnectionError();
    }
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::externalTransactionCreated(CryptoNote::TransactionId transactionId)  {
  WalletLegacyTransaction txInfo;
  m_wallet->getTransaction(transactionId, txInfo);
  
  std::stringstream logPrefix;
  if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
    logPrefix << "[ " << makeCenteredString(14, "UNCONFIRMED") << " ]";
  } else {
    logPrefix << "[ HEIGHT " << std::setw(BLOCK_MAX_WIDTH) << txInfo.blockHeight << " ]";
  }

  if (txInfo.totalAmount >= 0) {
      logger(INFO, GREEN)
      << "  " << std::setw(8) << "RECEIVED"
      << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(txInfo.totalAmount)
      << "  " << std::setw(HASH_MAX_WIDTH) << Common::podToHex(txInfo.hash)
      << "  " << logPrefix.str() ;

  } else {
      logger(INFO, MAGENTA)
      << "  " << std::setw(8) << "SPENT"
      << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(txInfo.totalAmount)      
      << "  " << std::setw(HASH_MAX_WIDTH) << Common::podToHex(txInfo.hash)
      << "  " << logPrefix.str()   ;
  }

  if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
    m_refresh_progress_reporter.update(m_node->getLastLocalBlockHeight(), true);
  } else {
    m_refresh_progress_reporter.update(txInfo.blockHeight, true);
  }
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::actualBalanceUpdated(uint64_t actualBalance) {
    // std::cout << "Actual balance updated :: " << m_currency.formatAmount(actualBalance) << ENDL;
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::synchronizationCompleted(std::error_code result) {
  std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
  m_walletSynchronized = true;
  m_walletSynchronizedCV.notify_one();
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::synchronizationProgressUpdated(uint32_t current, uint32_t total) {
  std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
  if (!m_walletSynchronized) {
    m_refresh_progress_reporter.update(current, false);
  }
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::export_keys(const std::vector<std::string>& args/* = std::vector<std::string>()*/) {
  AccountKeys keys;
  m_wallet->getAccountKeys(keys);
  std::string electrum_words;

  Crypto::secret_key_to_public_key(keys.spendSecretKey, keys.address.spendPublicKey);
  Crypto::secret_key_to_public_key(keys.viewSecretKey, keys.address.viewPublicKey);

  std::string private_key =  Common::podToHex(keys.address.spendPublicKey) + Common::podToHex(keys.address.viewPublicKey) + 
                             Common::podToHex(keys.spendSecretKey) + Common::podToHex(keys.viewSecretKey);
  std::string tracking_key = Common::podToHex(keys.address.spendPublicKey) + Common::podToHex(keys.address.viewPublicKey) + 
                            "0000000000000000000000000000000000000000000000000000000000000000" + Common::podToHex(keys.viewSecretKey);

  bool success = m_wallet->getSeed(electrum_words);


  success_msg_writer(true) << "WALLET ADDRESS         # " <<  m_wallet->getAddress();
  success_msg_writer(true) << "SPEND SECRET KEY [CLI] # " <<  Common::podToHex(keys.spendSecretKey);
  success_msg_writer(true) << "VIEW SECRET KEY  [CLI] # " <<  Common::podToHex(keys.viewSecretKey);
  success_msg_writer(true) << "PRIVATE KEY      [GUI] # " <<  private_key;
  success_msg_writer(true) << "TRACKING KEY     [GUI] # " <<  tracking_key;
  if (success) {
      success_msg_writer(true) << "MNEOMINC SEED WORDS    # " << electrum_words << std::endl;
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::create_integrated(const std::vector<std::string>& args /* = std::vector<std::string>()*/) {
    /* check if there is a payment id */
    std::string paymentID;
    if (args.empty()) {
        logger ( INFO , GREEN ) 
          << "Generatging with default payment ID ... ";
        paymentID = generatePaymentID();
    } else {
      paymentID = args[0];
    }

     
    std::string address = m_wallet->getAddress();
    uint64_t prefix;
    CryptoNote::AccountPublicAddress addr;

    /* get the spend and view public keys from the address */
    const bool valid = CryptoNote::parseAccountAddressString(prefix, addr, address);
    if (valid) {}
    CryptoNote::BinaryArray ba;
    CryptoNote::toBinaryArray(addr, ba);
    std::string keys = Common::asString(ba);

    /* create the integrated address the same way you make a public address */
    std::string integratedAddress = Tools::Base58::encode_addr(CryptoNote::parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX, paymentID + keys);

    logger(INFO, GREEN ) 
      << "PAYMENT ID      # " << paymentID << ENDL
      << "INTEGRATED ADDR # " << integratedAddress;

    return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_incoming_transfers(const std::vector<std::string>& args) {
  bool hasTransfers = false;
  size_t transactionsCount = m_wallet->getTransactionCount();

  std::string header = makeCenteredString(TIMESTAMP_MAX_WIDTH, "RCVD TIME (UTC)") + "  ";
  header += makeCenteredString(HASH_MAX_WIDTH, "HASH") + "  ";
  header += makeCenteredString(TOTAL_AMOUNT_MAX_WIDTH, "AMOUNT") + "  ";
  header += makeCenteredString(FEE_MAX_WIDTH, "FEE") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "BLOCK") + "  ";

  logger(INFO) << std::string(header.size(), '-');
  logger(INFO) << header;
  logger(INFO) << std::string(header.size(), '-');

  for (size_t transactionNumber = 0; transactionNumber < transactionsCount; ++transactionNumber) {
    WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(transactionNumber, txInfo);
    if (txInfo.totalAmount < 0) continue;
    hasTransfers = true;

    time_t timestamp = static_cast<time_t>(txInfo.timestamp);
    char timeString[TIMESTAMP_MAX_WIDTH + 1];
    if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", std::gmtime(&timestamp)) == 0) {
        throw std::runtime_error("time buffer is too small");
    }  

 std::string timeStr (timeString);
    std::string heightStr = std::to_string(txInfo.blockHeight);
    if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
      timeStr = "....-..-.. ..:..:..";
      heightStr = "UNCNFM";
    }
    
    logger(INFO, GREEN)
      << std::setw(TIMESTAMP_MAX_WIDTH) << timeStr
      << "  " << std::setw(HASH_MAX_WIDTH) << Common::podToHex(txInfo.hash) 
      << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(txInfo.totalAmount)
      << "  " << std::setw(FEE_MAX_WIDTH) << m_currency.formatAmount(txInfo.fee)
      << "  " << std::setw(BLOCK_MAX_WIDTH) << heightStr;

    if ( txInfo.messages.size() > 0) {
      for(auto message: txInfo.messages){
        logger(INFO, GREEN)
          << message << ENDL;
      }
    }
  }

  if (!hasTransfers) success_msg_writer() << "No incoming transfers";
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_outgoing_transfers(const std::vector<std::string>& args) {
  bool hasTransfers = false;
  size_t transactionsCount = m_wallet->getTransactionCount();
  // logger(INFO) << "        AMOUNT       \t                              TX ID";

  std::string header = makeCenteredString(TIMESTAMP_MAX_WIDTH, "SENT TIME (UTC)") + "  ";
  header += makeCenteredString(HASH_MAX_WIDTH, "HASH") + "  ";
  header += makeCenteredString(TOTAL_AMOUNT_MAX_WIDTH, "AMOUNT") + "  ";
  header += makeCenteredString(FEE_MAX_WIDTH, "FEE") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "BLOCK") + "  ";

  logger(INFO) << std::string(header.size(), '-');
  logger(INFO) << header;
  logger(INFO) << std::string(header.size(), '-');

  for (size_t transactionNumber = 0; transactionNumber < transactionsCount; ++transactionNumber) {
    WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(transactionNumber, txInfo);
    if (txInfo.totalAmount > 0) continue;
    hasTransfers = true;

    time_t timestamp = static_cast<time_t>(txInfo.timestamp);
    char timeString[TIMESTAMP_MAX_WIDTH + 1];
    if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", std::gmtime(&timestamp)) == 0) {
        throw std::runtime_error("time buffer is too small");
    }
    
    std::string timeStr (timeString);
    std::string heightStr = std::to_string(txInfo.blockHeight);
    if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
      timeStr = "....-..-.. ..:..:..";
      heightStr = "UNCNFM";
    }

    std::string txHash;

    if (static_cast<int64_t>(txInfo.depositCount) > 0) {
        txHash = " ^" + Common::podToHex(txInfo.hash);
    } else {
        txHash = "  " + Common::podToHex(txInfo.hash);
    }
    // std::cout << "deposit count " << txInfo.depositCount;
    // for ( auto i=0; i< txInfo.depositCount; i++){
    // std::cout << "deposit id    " << txInfo.firstDepositId;
    // }

    // std::string rowColor = static_cast<int64_t>( txInfo.depositCount ) < 1 ? MAGENTA : BRIGHT_GREEN;

    logger(INFO, MAGENTA)
      << std::setw(TIMESTAMP_MAX_WIDTH) << timeStr
      << "" << std::setw(HASH_MAX_WIDTH) << txHash
      << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(txInfo.totalAmount)
      << "  " << std::setw(FEE_MAX_WIDTH) << m_currency.formatAmount(txInfo.fee)
      << "  " << std::setw(BLOCK_MAX_WIDTH) << heightStr;

    for (TransferId id = txInfo.firstTransferId; id < txInfo.firstTransferId + txInfo.transferCount; ++id) {
      WalletLegacyTransfer tr;
      m_wallet->getTransfer(id, tr);
      logger(INFO, CYAN) 
        << "- " << std::setw(TIMESTAMP_MAX_WIDTH + HASH_MAX_WIDTH) << tr.address ;
      logger(INFO, MAGENTA)
        << std::setw(TIMESTAMP_MAX_WIDTH) << "  "
        << "  " << std::setw(HASH_MAX_WIDTH) << "  "
        << "  " << std::right << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(-tr.amount);
    }
    }

  if (!hasTransfers) success_msg_writer() << "No outgoing transfers";
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_balance(const std::vector<std::string>& args/* = std::vector<std::string>()*/) {
  logger(INFO, GREEN)        << "   AVAILABLE # " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(m_wallet->actualBalance())         << " [ WALLET  ] ";
  logger(INFO, YELLOW)       << "      LOCKED # " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(m_wallet->pendingBalance())        << " [ WALLET  ] ";
  logger(INFO, BRIGHT_GREEN) << "    UNLOCKED # " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(m_wallet->actualDepositBalance())  << " [ DEPOSIT ] ";
  logger(INFO, YELLOW)       << "     PENDING # " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(m_wallet->pendingDepositBalance()) << " [ DEPOSIT ] ";
  logger(INFO, BRIGHT_GREEN) << "TOTAL AMOUNT # " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(m_wallet->actualBalance() + m_wallet->pendingBalance() + m_wallet->actualDepositBalance() + m_wallet->pendingDepositBalance());

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::listTransfers(const std::vector<std::string>& args) {
  bool haveTransfers = false;

  size_t transactionsCount = m_wallet->getTransactionCount();
  for (size_t transactionNumber = 0; transactionNumber < transactionsCount; ++transactionNumber) {
    WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(transactionNumber, txInfo);
    if (txInfo.state != WalletLegacyTransactionState::Active || txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
      continue;
    }

    if (!haveTransfers) {
      printListTransfersHeader(logger);
      haveTransfers = true;
    }

    printListTransfersItem(logger, txInfo, *m_wallet, m_currency);
  }

  if (!haveTransfers) {
    success_msg_writer() << "No transfers";
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_payments(const std::vector<std::string> &args) {
  if (args.empty()) {
    fail_msg_writer() << "Expected at least one payment ID";
    return true;
  }
  std::string header = makeCenteredString(TIMESTAMP_MAX_WIDTH, "TIME (UTC)") + "  ";
  header += makeCenteredString(HASH_MAX_WIDTH, "TX HASH") + "  ";
  header += makeCenteredString(TOTAL_AMOUNT_MAX_WIDTH, "AMOUNT") + "  ";
  header += makeCenteredString(FEE_MAX_WIDTH, "FEE") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "BLOCK") + "  ";

  logger(INFO) << std::string(header.size(), '-');
  logger(INFO) << header;
  logger(INFO) << std::string(header.size(), '-');

  try {
    auto hashes = args;
    std::sort(std::begin(hashes), std::end(hashes));
    hashes.erase(std::unique(std::begin(hashes), std::end(hashes)), std::end(hashes));
    std::vector<PaymentId> paymentIds;
    paymentIds.reserve(hashes.size());
    std::transform(std::begin(hashes), std::end(hashes), std::back_inserter(paymentIds), [](const std::string& arg) {
      PaymentId paymentId;
      if (!CryptoNote::parsePaymentId(arg, paymentId)) {
        throw std::runtime_error("payment ID has invalid format: \"" + arg + "\", expected 64-character string");
      }

      return paymentId;
    });

    auto payments = m_wallet->getTransactionsByPaymentIds(paymentIds);

    for (auto& payment : payments) {
      logger(INFO, BRIGHT_YELLOW)
        << std::setw(TIMESTAMP_MAX_WIDTH) << "PAYMENT ID"
        << "  " << std::setw(HASH_MAX_WIDTH) << Common::podToHex(payment.paymentId);
      for (auto& txInfo : payment.transactions) {
        time_t timestamp = static_cast<time_t>(txInfo.timestamp);
        char timeString[TIMESTAMP_MAX_WIDTH + 1];
        if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", std::gmtime(&timestamp)) == 0) {
            throw std::runtime_error("time buffer is too small");
        }
        
        std::string timeStr (timeString);
        std::string heightStr = std::to_string(txInfo.blockHeight);
        if (txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
          timeStr = "....-..-.. ..:..:..";
          heightStr = "UNCNFM";
        }

        logger(INFO, YELLOW)
          << std::setw(TIMESTAMP_MAX_WIDTH) << timeStr
          << "  " << std::setw(HASH_MAX_WIDTH) << Common::podToHex(txInfo.hash)
          << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(txInfo.totalAmount)
          << "  " << std::setw(FEE_MAX_WIDTH) << m_currency.formatAmount(txInfo.fee)
          << "  " << std::setw(BLOCK_MAX_WIDTH) << std::to_string(txInfo.blockHeight);
      }

      if (payment.transactions.empty()) {
        success_msg_writer() << "No payments with id " << Common::podToHex(payment.paymentId);
      }
    }
  } catch (std::exception& e) {
    fail_msg_writer() << "show_payments exception: " << e.what();
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_blockchain_height(const std::vector<std::string>& args) {
  try {
    uint64_t bc_height = m_node->getLastLocalBlockHeight();
    success_msg_writer() << bc_height;
  } catch (std::exception &e) {
    fail_msg_writer() << "failed to get blockchain height: " << e.what();
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
std::string simple_wallet::resolveAlias(const std::string& aliasUrl) {
	std::string host;
	std::string uri;
	std::vector<std::string>records;
	std::string address;

	if (!Common::fetch_dns_txt(aliasUrl, records)) {
		throw std::runtime_error("Failed to lookup DNS record");
	}

	for (const auto& record : records) {
		if (processServerAliasResponse(record, address)) {
			return address;
		}
	}
	throw std::runtime_error("Failed to parse server response");
}

//----------------------------------------------------------------------------------------------------
std::string simple_wallet::getFeeAddress() {
  
  HttpClient httpClient(m_dispatcher, m_daemon_host, m_daemon_port);

  HttpRequest req;
  HttpResponse res;

  req.setUrl("/feeaddress");
  try {
	  httpClient.request(req, res);
  }
  catch (const std::exception& e) {
	  fail_msg_writer() << "Error connecting to the remote node: " << e.what();
  }

  if (res.getStatus() != HttpResponse::STATUS_200) {
	  fail_msg_writer() << "Remote node returned code " + std::to_string(res.getStatus());
  }

  std::string address;
  if (!processServerFeeAddressResponse(res.getBody(), address)) {
	  fail_msg_writer() << "Failed to parse remote node response";
  }

  return address;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_unlocked_outputs(const std::vector<std::string>& args) {
  std::string header = makeCenteredString(HASH_MAX_WIDTH, "HASH") + "  ";
  header += makeCenteredString(TOTAL_AMOUNT_MAX_WIDTH, "AMOUNT") + "  ";
  uint64_t total = 0;
  try {
      logger(INFO) << std::string(header.size(), '-');
      logger(INFO) << header;
      logger(INFO) << std::string(header.size(), '-');
      std::vector<TransactionOutputInformation> outputs = m_wallet->getUnlockedOutputs();
      for (auto output : outputs) {
          logger(INFO, GREEN) 
            << "  " << std::setw(HASH_MAX_WIDTH) << Common::podToHex(output.transactionHash) 
            << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(output.amount);
            total += output.amount;
      }
      logger(INFO) << std::string(header.size(), '-');
      logger(INFO, GREEN) 
        << "  " << std::right << std::setw(HASH_MAX_WIDTH) << "TOTAL AMOUNT"
        << "  " << std::right << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << m_currency.formatAmount(total);
      logger(INFO) << std::string(header.size(), '-');
      logger(INFO, GREEN) 
        << "  " << std::right << std::setw(HASH_MAX_WIDTH) << "OUTPUTS COUNT"
        << "  " << std::right << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << outputs.size();
      logger(INFO) << std::string(header.size(), '-');
  } catch (std::exception& e) {
      fail_msg_writer() << "failed to get outputs: " << e.what();
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::transfer(const std::vector<std::string> &args) {
  try {
    TransferCommand cmd(m_currency);

    if (!cmd.parseArguments(logger, args))
      return true;

    for (auto& kv: cmd.aliases) {
      std::string address;

      try {
        address = resolveAlias(kv.first);

        AccountPublicAddress ignore;
        if (!m_currency.parseAccountAddressString(address, ignore)) {
          throw std::runtime_error("Address \"" + address + "\" is invalid");
        }
      } catch (std::exception& e) {
        logger(ERROR, RED) << e.what() << ", Alias: " << kv.first;
        return true;
      }

      for (auto& transfer: kv.second) {
        transfer.address = address;
      }
    }

    if (!cmd.aliases.empty()) {
      if (!askAliasesTransfersConfirmation(cmd.aliases, m_currency, logger)) {
        return true;
      }

      for (auto& kv: cmd.aliases) {
        std::copy(std::move_iterator<std::vector<WalletLegacyTransfer>::iterator>(kv.second.begin()),
                  std::move_iterator<std::vector<WalletLegacyTransfer>::iterator>(kv.second.end()),
                  std::back_inserter(cmd.dsts));
      }
    }

    std::vector<TransactionMessage> messages;
    for (auto dst : cmd.dsts) {
      for (auto msg : cmd.messages) {
        messages.emplace_back(TransactionMessage{ msg, dst.address });
      }
    }

    uint64_t ttl = 0;
    if (cmd.ttl != 0) {
      ttl = static_cast<uint64_t>(time(nullptr)) + cmd.ttl;
    }

    CryptoNote::WalletHelper::SendCompleteResultObserver sent;

    std::string extraString;
    std::copy(cmd.extra.begin(), cmd.extra.end(), std::back_inserter(extraString));

    WalletHelper::IWalletRemoveObserverGuard removeGuard(*m_wallet, sent);

    CryptoNote::TransactionId tx = m_wallet->sendTransaction(cmd.dsts, cmd.fee, extraString, cmd.fake_outs_count, 0, messages, ttl);
    if (tx == WALLET_LEGACY_INVALID_TRANSACTION_ID) {
      fail_msg_writer() << "Can't send money";
      return true;
    }

    std::error_code sendError = sent.wait(tx);
    removeGuard.removeObserver();

    if (sendError) {
      fail_msg_writer() << sendError.message();
      return true;
    }

    CryptoNote::WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(tx, txInfo);
    logger(INFO, GREEN) << "Money successfully sent" << ENDL;
    logger(INFO, CYAN)  << "   TX ID  # " << Common::podToHex(txInfo.hash) ;
    logger(INFO, CYAN)  << "   TX KEY # " << Common::podToHex(txInfo.secretKey);

    try {
      CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    } catch (const std::exception& e) {
      fail_msg_writer() << e.what();
      return true;
    }
  } catch (const std::system_error& e) {
    fail_msg_writer() << e.what();
  } catch (const std::exception& e) {
    fail_msg_writer() << e.what();
  } catch (...) {
    fail_msg_writer() << "unknown error";
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::run() {
  {
    std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
    while (!m_walletSynchronized) {
      m_walletSynchronizedCV.wait(lock);
    }
  }

  std::cout << std::endl;

  std::string addr_start = m_wallet->getAddress().substr(0, 6);
  m_consoleHandler.start(false, "[wallet " + addr_start + "]: ", Common::Console::Color::BrightYellow);
  return true;
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::stop() {
  m_consoleHandler.requestStop();
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::print_address(const std::vector<std::string> &args/* = std::vector<std::string>()*/) {
  success_msg_writer() << m_wallet->getAddress();
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::sign_message(const std::vector<std::string> &args) {
  if (args.size() != 1) {
    fail_msg_writer() << "usage: sign \"message to sign\" (use quotes if case of spaces)";
    return true;
  }
  if (m_trackingWallet) {
    fail_msg_writer() << "wallet is watch-only and cannot sign";
    return true;
  }
  std::string message = args[0];
  std::string signature = m_wallet->sign_message(message);
  success_msg_writer() << signature;
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::verify_message(const std::vector<std::string> &args) {
  if (args.size() != 3) {
    fail_msg_writer() << "usage: verify \"message to verify\" <address> <signature>";
    return true;
  }
  std::string message = args[0];
  std::string address_string = args[1];
  std::string signature = args[2];
  CryptoNote::AccountPublicAddress address;
  if (!m_currency.parseAccountAddressString(address_string, address)) {
    fail_msg_writer() << "failed to parse address " << address_string;
	return true;
  }
  const size_t header_len = strlen("SigV1");
  if (signature.size() < header_len || signature.substr(0, header_len) != "SigV1") {
    fail_msg_writer() << ("Signature header check error");
    return false;
  }
  std::string decoded;
  if (!Tools::Base58::decode(signature.substr(header_len), decoded)) {
    fail_msg_writer() << ("Signature decoding error");
    return false;
  }
  Crypto::Signature s;
  if (sizeof(s) != decoded.size()) {
    fail_msg_writer() << ("Signature decoding error");
    return false;
  }
  bool r = m_wallet->verify_message(message, address, signature);
  if (!r) {
    fail_msg_writer() << "Invalid signature from " << address_string;
  } else {
    success_msg_writer() << "Valid signature from " << address_string;
  }
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::process_command(const std::vector<std::string> &args) {
  return m_consoleHandler.runCommand(args);
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::printConnectionError() const {
  fail_msg_writer() << "wallet failed to connect to daemon (" << m_daemon_address << ").";
}


int main(int argc, char* argv[]) {
#ifdef WIN32
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  po::options_description desc_general("General options");
  command_line::add_arg(desc_general, command_line::arg_help);
  command_line::add_arg(desc_general, command_line::arg_version);

  po::options_description desc_params("Wallet options");
  command_line::add_arg(desc_params, arg_wallet_file);
  command_line::add_arg(desc_params, arg_generate_new_wallet);
  command_line::add_arg(desc_params, arg_restore_deterministic_wallet);
  command_line::add_arg(desc_params, arg_non_deterministic);
  command_line::add_arg(desc_params, arg_mnemonic_seed);
  command_line::add_arg(desc_params, arg_password);
  command_line::add_arg(desc_params, arg_daemon_address);
  command_line::add_arg(desc_params, arg_daemon_host);
  command_line::add_arg(desc_params, arg_daemon_port);
  command_line::add_arg(desc_params, arg_command);
  command_line::add_arg(desc_params, arg_log_level);
  command_line::add_arg(desc_params, arg_testnet);
  Tools::wallet_rpc_server::init_options(desc_params);

  po::positional_options_description positional_options;
  positional_options.add(arg_command.name, -1);

  po::options_description desc_all;
  desc_all.add(desc_general).add(desc_params);

  Logging::LoggerManager logManager;
  Logging::LoggerRef logger(logManager, "simplewallet");
  System::Dispatcher dispatcher;

  po::variables_map vm;

  bool r = command_line::handle_error_helper(desc_all, [&]() {
    po::store(command_line::parse_command_line(argc, argv, desc_general, true), vm);

    if (command_line::get_arg(vm, command_line::arg_help)) {
      CryptoNote::Currency tmp_currency = CryptoNote::CurrencyBuilder(logManager).currency();
      CryptoNote::simple_wallet tmp_wallet(dispatcher, tmp_currency, logManager);

      std::cout << CRYPTONOTE_NAME << " wallet v" << PROJECT_VERSION_LONG << std::endl;
      std::cout << "Usage: simplewallet [--wallet-file=<file>|--generate-new-wallet=<file>] [--daemon-address=<host>:<port>] [<COMMAND>]";
      std::cout << desc_all << '\n' << tmp_wallet.get_commands_str();
      return false;
    } else if (command_line::get_arg(vm, command_line::arg_version))  {
      std::cout << CRYPTONOTE_NAME << " wallet v" << PROJECT_VERSION_LONG;
      return false;
    }

    auto parser = po::command_line_parser(argc, argv).options(desc_params).positional(positional_options);
    po::store(parser.run(), vm);
    po::notify(vm);
    return true;
  });

  if (!r)
    return 1;

  //set up logging options
  Level logLevel = INFO;

  if (command_line::has_arg(vm, arg_log_level)) {
    logLevel = static_cast<Level>(command_line::get_arg(vm, arg_log_level));
  }

  logManager.configure(buildLoggerConfiguration(logLevel, Common::ReplaceExtenstion(argv[0], ".log")));

  logger(INFO, BRIGHT_WHITE) << CRYPTONOTE_NAME << " wallet v" << PROJECT_VERSION_LONG;

  CryptoNote::Currency currency = CryptoNote::CurrencyBuilder(logManager).
    testnet(command_line::get_arg(vm, arg_testnet)).currency();

  if (command_line::has_arg(vm, Tools::wallet_rpc_server::arg_rpc_bind_port)) {
    //runs wallet with rpc interface
    if (!command_line::has_arg(vm, arg_wallet_file)) {
      logger(ERROR, BRIGHT_RED) << "Wallet file not set.";
      return 1;
    }

    if (!command_line::has_arg(vm, arg_daemon_address)) {
      logger(ERROR, BRIGHT_RED) << "Daemon address not set.";
      return 1;
    }

    if (!command_line::has_arg(vm, arg_password)) {
      logger(ERROR, BRIGHT_RED) << "Wallet password not set.";
      return 1;
    }

    std::string wallet_file = command_line::get_arg(vm, arg_wallet_file);
    std::string wallet_password = command_line::get_arg(vm, arg_password);
    std::string daemon_address = command_line::get_arg(vm, arg_daemon_address);
    std::string daemon_host = command_line::get_arg(vm, arg_daemon_host);
    uint16_t daemon_port = command_line::get_arg(vm, arg_daemon_port);
    if (daemon_host.empty())
      daemon_host = "localhost";
    if (!daemon_port)
      daemon_port = RPC_DEFAULT_PORT;

    if (!daemon_address.empty()) {
      if (!parseUrlAddress(daemon_address, daemon_host, daemon_port)) {
        logger(ERROR, BRIGHT_RED) << "failed to parse daemon address: " << daemon_address;
        return 1;
      }
    }

    std::unique_ptr<INode> node(new NodeRpcProxy(daemon_host, daemon_port));

    std::promise<std::error_code> errorPromise;
    std::future<std::error_code> error = errorPromise.get_future();
    auto callback = [&errorPromise](std::error_code e) {errorPromise.set_value(e); };
    node->init(callback);
    if (error.get()) {
      logger(ERROR, BRIGHT_RED) << ("failed to init NodeRPCProxy");
      return 1;
    }

    std::unique_ptr<IWalletLegacy> wallet(new WalletLegacy(currency, *node.get(), logManager));
    std::string walletFileName;
    try  {
      walletFileName = ::tryToOpenWalletOrLoadKeysOrThrow(logger, wallet, wallet_file, wallet_password);

      logger(INFO) << "available balance: " << currency.formatAmount(wallet->actualBalance()) <<
      ", locked amount: " << currency.formatAmount(wallet->pendingBalance());

      logger(INFO, BRIGHT_GREEN) << "Loaded ok";
    } catch (const std::exception& e)  {
      logger(ERROR, BRIGHT_RED) << "Wallet initialize failed: " << e.what();
      return 1;
    }

    Tools::wallet_rpc_server wrpc(dispatcher, logManager, *wallet, *node, currency, walletFileName);

    if (!wrpc.init(vm)) {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize wallet rpc server";
      return 1;
    }

    Tools::SignalHandler::install([&wrpc] {
      wrpc.send_stop_signal();
    });

    logger(INFO) << "Starting wallet rpc server";
    wrpc.run();
    logger(INFO) << "Stopped wallet rpc server";
    
    try {
      logger(INFO) << "Storing wallet...";
      CryptoNote::WalletHelper::storeWallet(*wallet, walletFileName);
      logger(INFO, BRIGHT_GREEN) << "Stored ok";
    } catch (const std::exception& e) {
      logger(ERROR, BRIGHT_RED) << "Failed to store wallet: " << e.what();
      return 1;
    }
  } else {
    //runs wallet with console interface
    CryptoNote::simple_wallet wal(dispatcher, currency, logManager);
    
    if (!wal.init(vm)) {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize wallet"; 
      return 1; 
    }

    std::vector<std::string> command = command_line::get_arg(vm, arg_command);
    if (!command.empty())
      wal.process_command(command);

    Tools::SignalHandler::install([&wal] {
      wal.stop();
    });
    
    wal.run();

    if (!wal.deinit()) {
      logger(ERROR, BRIGHT_RED) << "Failed to close wallet";
    } else {
      logger(INFO) << "Wallet closed";
    }
  }
  return 1;
  //CATCH_ENTRY_L0("main", 1);
}


