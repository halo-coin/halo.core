// Copyright (c) 2018-2019 Halo Sphere developers 
// Copyright (c) 2011-2016 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <functional>
#include <iostream>
#include <cstring>
#include <string>
#include <map>
#include <boost/program_options/variables_map.hpp>
#include <iostream>
#ifdef _WIN32
#include <Windows.h>
#include <io.h>
//#include <crtdbg.h>
//#include <winsock2.h>
#include <windns.h>
#include <Rpc.h>
#else
#include <arpa/nameser.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <resolv.h>
#include <netdb.h>
#include <unistd.h>
#endif
#include "DnsTools.h"

namespace Common {

#ifndef __ANDROID__

	bool fetch_dns_txt(const std::string domain, std::vector<std::string>&records) {

#ifdef _WIN32
		using namespace std;

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Dnsapi.lib")

		PDNS_RECORD pDnsRecord;          //Pointer to DNS_RECORD structure.

		{
			WORD type = DNS_TYPE_TEXT;

			if (0 != DnsQuery_A(domain.c_str(), type, DNS_QUERY_BYPASS_CACHE, NULL, &pDnsRecord, NULL))
			{
				cerr << "Error querying: '" << domain << "'" << endl;
				return false;
			}
		}

		PDNS_RECORD it;
		map<WORD, function<void(void)>> callbacks;

		callbacks[DNS_TYPE_TEXT] = [&it, &records](void) -> void {
			std::stringstream stream;
			for (DWORD i = 0; i < it->Data.TXT.dwStringCount; i++) {
				stream << RPC_CSTR(it->Data.TXT.pStringArray[i]) << endl;;
			}
			records.push_back(stream.str());
		};

		for (it = pDnsRecord; it != NULL; it = it->pNext) {
			if (callbacks.count(it->wType)) {
				callbacks[it->wType]();
			}
		}
		DnsRecordListFree(pDnsRecord, DnsFreeRecordListDeep);
#else
		using namespace std;

		res_init();
		ns_msg nsMsg;
		int response;
		unsigned char query_buffer[4096];
		{
			ns_type type = ns_t_txt;

			const char * c_domain = (domain).c_str();
			response = res_query(c_domain, 1, type, query_buffer, sizeof(query_buffer));

			if (response < 0)
				return false;
		}

		ns_initparse(query_buffer, response, &nsMsg);

		map<ns_type, function<void(const ns_rr &rr)>> callbacks;

		callbacks[ns_t_txt] = [&nsMsg, &records](const ns_rr &rr) -> void {
			int txt_len = *(unsigned char *) ns_rr_rdata(rr);
			char txt[256];
			memset(txt, 0, 256);
			if (txt_len <= 255){
				memcpy(txt, ns_rr_rdata(rr) + 1, txt_len);
				records.push_back(txt);
			}
		};

		for (int x = 0; x < ns_msg_count(nsMsg, ns_s_an); x++) {
			ns_rr rr;
			ns_parserr(&nsMsg, ns_s_an, x, &rr);
			ns_type type = ns_rr_type(rr);
			if (callbacks.count(type)) {
				callbacks[type](rr);
			}
		}

#endif
		if (records.empty())
			return false;

		return true;
	}

#endif

}
