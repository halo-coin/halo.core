// Copyright (c) 2018-2019 Halo Sphere developers 
// Copyright (c) 2011-2016 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "crypto/crypto.h"

#pragma once

namespace CryptoNote {

  class ISerializer;

  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  class AccountBase {
  public:
    AccountBase();
    void generate();
    void              generateDeterministic();
    Crypto::SecretKey generate_key(const Crypto::SecretKey& recovery_key = Crypto::SecretKey(), bool recover = false, bool two_random = false);

    static void generateViewFromSpend(Crypto::SecretKey&, Crypto::SecretKey&, Crypto::PublicKey&);
    static void generateViewFromSpend(Crypto::SecretKey&, Crypto::SecretKey&);

    const AccountKeys& getAccountKeys() const;
    void setAccountKeys(const AccountKeys& keys);
    uint64_t get_createtime() const { return m_creation_timestamp; }
    void set_createtime(uint64_t val) { m_creation_timestamp = val; }
    void serialize(ISerializer& s);

    template <class t_archive>
    inline void serialize(t_archive &a, const unsigned int /*ver*/) {
      a & m_keys;
      a & m_creation_timestamp;
    }

  private:
    void setNull();
    AccountKeys m_keys;
    uint64_t m_creation_timestamp;
  };
}
