// Copyright (c) 2018-2019 Halo Sphere developers 
// Copyright (c) 2011-2016 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once
#include "CryptoNoteProtocol/CryptoNoteProtocolDefinitions.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "crypto/hash.h"
#include "Rpc/CoreRpcServerCommandsDefinitions.h"
#include "WalletRpcServerErrorCodes.h"

namespace Tools
{
namespace wallet_rpc
{

using CryptoNote::ISerializer;

#define WALLET_RPC_STATUS_OK      "OK"
#define WALLET_RPC_STATUS_BUSY    "BUSY"

  struct COMMAND_RPC_GET_BALANCE
  {
    typedef CryptoNote::EMPTY_STRUCT request;

    struct response
    {
      uint64_t locked_amount;
      uint64_t available_balance;
      uint64_t balance;            //<! \deprecated Use locked_amount + available_balance
      uint64_t unlocked_balance;   //<! \deprecated Use available_balance

      void serialize(ISerializer& s) {
        KV_MEMBER(locked_amount)
        KV_MEMBER(available_balance)
        KV_MEMBER(balance)
        KV_MEMBER(unlocked_balance)
      }
    };
  };

  struct transfer_destination
  {
    uint64_t amount;
    std::string address;
    std::string message;

    void serialize(ISerializer& s) {
      KV_MEMBER(amount)
      KV_MEMBER(address)
      KV_MEMBER(message)
    }
  };

  struct TransferMessage {
    std::string address;
    std::string message;

    void serialize(ISerializer& s) {
      KV_MEMBER(address)
      KV_MEMBER(message)
    }
  };

  struct COMMAND_RPC_TRANSFER
  {
    struct request
    {
      std::list<transfer_destination> destinations;
      uint64_t fee;
      uint64_t mixin;
      uint64_t unlock_time;
      std::string payment_id;
      std::list<TransferMessage> messages;
      uint64_t ttl = 0;

      void serialize(ISerializer& s) {
        KV_MEMBER(destinations)
        KV_MEMBER(fee)
        KV_MEMBER(mixin)
        KV_MEMBER(unlock_time)
        KV_MEMBER(payment_id)
        KV_MEMBER(messages)
        KV_MEMBER(ttl)
      }
    };

    struct response
    {
      std::string tx_hash;
			std::string tx_key;

      void serialize(ISerializer& s) {
        KV_MEMBER(tx_hash)
      	KV_MEMBER(tx_key)

      }
    };
  };

  struct COMMAND_RPC_STORE
  {
    typedef CryptoNote::EMPTY_STRUCT request;
    typedef CryptoNote::EMPTY_STRUCT response;
  };

	/* Command: stop_wallet */
	struct COMMAND_RPC_STOP
	{
		typedef CryptoNote::EMPTY_STRUCT request;
		typedef CryptoNote::EMPTY_STRUCT response;
	};

  struct transaction_messages {
    std::string tx_hash;
    uint64_t tx_id;
    uint32_t block_height;
    uint64_t timestamp;
    std::list<std::string> messages;

    void serialize(ISerializer& s) {
      KV_MEMBER(tx_hash);
      KV_MEMBER(tx_id);
      KV_MEMBER(block_height);
      KV_MEMBER(timestamp);
      KV_MEMBER(messages);
    }
  };

  struct COMMAND_RPC_GET_MESSAGES {
    struct request {
      uint64_t first_tx_id = 0;
      uint32_t tx_limit = std::numeric_limits<uint32_t>::max();

      void serialize(ISerializer& s) {
        KV_MEMBER(first_tx_id);
        KV_MEMBER(tx_limit);
      }
    };

    struct response {
      uint64_t total_tx_count;
      std::list<transaction_messages> tx_messages;

      void serialize(ISerializer& s) {
        KV_MEMBER(total_tx_count);
        KV_MEMBER(tx_messages);
      }
    };
  };

  struct payment_details
  {
    std::string tx_hash;
    uint64_t amount;
    uint64_t block_height;
    uint64_t unlock_time;

    void serialize(ISerializer& s) {
      KV_MEMBER(tx_hash)
      KV_MEMBER(amount)
      KV_MEMBER(block_height)
      KV_MEMBER(unlock_time)
    }
  };

  struct outputs_details
  {
    std::string tx_hash;
    uint64_t amount;

    void serialize(ISerializer& s) {
      KV_MEMBER(tx_hash)
      KV_MEMBER(amount)
    }
  };

  struct COMMAND_RPC_GET_PAYMENTS
  {
    struct request
    {
      std::string payment_id;

      void serialize(ISerializer& s) {
        KV_MEMBER(payment_id)
      }
    };

    struct response
    {
      std::list<payment_details> payments;

      void serialize(ISerializer& s) {
        KV_MEMBER(payments)
      }
    };
  };

  struct Transfer {
    uint64_t time;
    bool output;
    std::string transactionHash;
    uint64_t amount;
    uint64_t fee;
    std::string paymentId;
    std::string address;
    uint64_t blockIndex;
    uint64_t unlockTime;
    uint64_t confirmations;
		std::string txKey;

    void serialize(ISerializer& s) {
      KV_MEMBER(time)
      KV_MEMBER(output)
      KV_MEMBER(transactionHash)
      KV_MEMBER(amount)
      KV_MEMBER(fee)
      KV_MEMBER(paymentId)
      KV_MEMBER(address)
      KV_MEMBER(blockIndex)
      KV_MEMBER(unlockTime)
      KV_MEMBER(confirmations)
      KV_MEMBER(txKey)
    }
  };

  struct COMMAND_RPC_GET_TRANSFERS {
    typedef CryptoNote::EMPTY_STRUCT request;

    struct response {
      std::list<Transfer> transfers;

      void serialize(ISerializer& s) {
        KV_MEMBER(transfers)
      }
    };
  };

	/* Command: get_transaction */
	struct COMMAND_RPC_GET_TRANSACTION
	{
		struct request
		{
			std::string tx_hash;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(tx_hash)
			}
		};
		struct response
		{
			Transfer transaction_details;
			std::list<transfer_destination> destinations;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(transaction_details)
				KV_MEMBER(destinations)
			}
		};
	};
  
  struct COMMAND_RPC_GET_HEIGHT {
    typedef CryptoNote::EMPTY_STRUCT request;

    struct response {
      uint64_t height;

      void serialize(ISerializer& s) {
        KV_MEMBER(height)
      }
    };
  };

  struct COMMAND_RPC_SIGN_MESSAGE
	{
		struct request
		{
			std::string message;
 
			void serialize(ISerializer& s)
			{
				KV_MEMBER(message);
			}
		};

		struct response
		{
			std::string signature;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(signature);
			}
		};
	};

	struct COMMAND_RPC_VERIFY_MESSAGE
	{
		struct request
		{
			std::string message;
			std::string address;
			std::string signature;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(message);
				KV_MEMBER(address);
				KV_MEMBER(signature);
			}
		};

		struct response
		{
			bool good;
 
			void serialize(ISerializer& s)
			{
				KV_MEMBER(good);
			}
		};
	};

  struct COMMAND_RPC_GET_TX_PROOF
  {
    struct request
    {
      std::string tx_hash;
      std::string dest_address;
      std::string tx_key;

      void serialize(ISerializer& s)
      {
        KV_MEMBER(tx_hash);
        KV_MEMBER(dest_address);
        KV_MEMBER(tx_key);
      }
    };

    struct response
    {
      std::string signature;

      void serialize(ISerializer& s)
      {
        KV_MEMBER(signature);
      }
    };
  };


	struct COMMAND_RPC_GET_BALANCE_PROOF
	{
		struct request
		{
			uint64_t amount = 0;
			std::string message;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(amount);
				KV_MEMBER(message);
			}
		};

		struct response
		{
			std::string signature;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(signature);
			}
		};
	};

  struct COMMAND_RPC_RESET {
    typedef CryptoNote::EMPTY_STRUCT request;
    typedef CryptoNote::EMPTY_STRUCT response;
  };

  struct COMMAND_RPC_QUERY_KEY {
    struct request
    {
      std::string key_type;

      void serialize(ISerializer& s) {
        KV_MEMBER(key_type)
      }
    };

    struct response
    {
        std::string key;

        void serialize(ISerializer& s) {
          KV_MEMBER(key)
        }
      };
  };

	/* Command: get_tx_key */
	struct COMMAND_RPC_GET_TX_KEY
	{
		struct request
		{
			std::string tx_hash;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(tx_hash)
			}
		};
		struct response
		{
			std::string tx_key;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(tx_key)
			}
		};
	};

	/* Command: get_address */
	struct COMMAND_RPC_GET_ADDRESS
	{
		typedef CryptoNote::EMPTY_STRUCT request;
		struct response
		{
			std::string address;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(address)
			}
		};
	};
  
  /* Command: paymentid */
	struct COMMAND_RPC_GEN_PAYMENT_ID
	{
		typedef CryptoNote::EMPTY_STRUCT request;
		struct response
		{
			std::string payment_id;

			void serialize(ISerializer& s)
			{
				KV_MEMBER(payment_id)
			}
		};
	};

  struct COMMAND_RPC_GET_OUTPUTS
  {
    typedef CryptoNote::EMPTY_STRUCT request;

    struct response
    {
      std::list<outputs_details> outputs;
      size_t total;
      size_t unlocked_outputs_count;

      void serialize(ISerializer& s) {
        KV_MEMBER(outputs);
        KV_MEMBER(total);
        KV_MEMBER(unlocked_outputs_count);
      }
    };
  };

}}
