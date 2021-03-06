//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2012-2016 OpenVPN Technologies, Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

#ifndef OPENVPN_SERVER_LISTENLIST_H
#define OPENVPN_SERVER_LISTENLIST_H

#include <string>
#include <vector>
#include <utility> // for std::move

#include <openvpn/common/platform.hpp>
#include <openvpn/common/exception.hpp>
#include <openvpn/common/options.hpp>
#include <openvpn/common/hostport.hpp>
#include <openvpn/common/number.hpp>
#include <openvpn/common/string.hpp>
#include <openvpn/common/format.hpp>
#include <openvpn/addr/ip.hpp>
#include <openvpn/transport/protocol.hpp>

namespace openvpn {
  namespace Listen {
    struct Item
    {
      enum SSLMode {
	SSLUnspecified,
	SSLOn,
	SSLOff,
      };

      std::string directive;
      std::string addr;
      std::string port;
      Protocol proto;
      SSLMode ssl = SSLUnspecified;
      unsigned int n_threads = 0;

      std::string to_string() const
      {
	std::ostringstream os;
	os << directive << ' ' << addr;
	if (!proto.is_local())
	  os << ' ' << port;
	os << ' ' << proto.str() << ' ' << n_threads;
	if (ssl == SSLOn)
	  os << " ssl";
	else if (ssl == SSLOff)
	  os << " !ssl";
	return os.str();
      }

      Item port_offset(const unsigned int offset) const
      {
	Item ret(*this);
	ret.port = openvpn::to_string(HostPort::parse_port(ret.port, "offset") + offset);
	return ret;
      }
    };

    class List : public std::vector<Item>
    {
    public:
      enum LoadMode {
	Nominal,
	AllowDefault,
	AllowEmpty
      };

      List() {}

      List(const Item& item)
      {
	push_back(item);
      }

      List(Item&& item)
      {
	push_back(std::move(item));
      }

      List(const OptionList& opt,
	   const std::string& directive,
	   const LoadMode load_mode,
	   const unsigned int n_cores)
      {
	size_t n_listen = 0;

	for (OptionList::const_iterator i = opt.begin(); i != opt.end(); ++i)
	  {
	    const Option& o = *i;
	    if (match(directive, o))
	      ++n_listen;
	  }

	if (n_listen)
	  {
	    reserve(n_listen);

	    for (OptionList::const_iterator i = opt.begin(); i != opt.end(); ++i)
	      {
		const Option& o = *i;
		if (match(directive, o))
		  {
		    o.touch();

		    unsigned int mult = 1;
		    int local = 0;

		    Item e;

		    // directive
		    e.directive = o.get(0, 64);

		    // IP address
		    e.addr = o.get(1, 128);

		    // port number
		    e.port = o.get(2, 16);
		    if (Protocol::is_local_type(e.port))
		      {
			local = 1;
			e.port = "";
		      }
		    else
		      HostPort::validate_port(e.port, e.directive);

		    // protocol
		    {
		      const std::string title = e.directive + " protocol";
		      e.proto = Protocol::parse(o.get(3-local, 16), Protocol::NO_SUFFIX, title.c_str());
		    }
		    if (!local)
		      {
			// modify protocol based on IP version of given address
			const std::string title = e.directive + " addr";
			const IP::Addr addr = IP::Addr(e.addr, title.c_str());
			e.proto.mod_addr_version(addr);
		      }

		    // number of threads
		    int n_threads_exists = 0;
		    {
		      const std::string ntstr = o.get_optional(4-local, 16);
		      if (ntstr.length() > 0 && string::is_digit(ntstr[0]))
			n_threads_exists = 1;
		    }
		    if (n_threads_exists)
		      {
			std::string n_threads = o.get(4-local, 16);
			if (string::ends_with(n_threads, "*N"))
			  {
			    mult = n_cores;
			    n_threads = n_threads.substr(0, n_threads.length() - 2);
			  }
			if (!parse_number_validate<unsigned int>(n_threads, 3, 1, 100, &e.n_threads))
			  OPENVPN_THROW(option_error, e.directive << ": bad num threads: " << n_threads);
#ifndef OPENVPN_PLATFORM_WIN
			if (local && e.n_threads != 1)
			  OPENVPN_THROW(option_error, e.directive << ": local socket only supports one thread per pathname (not " << n_threads << ')');
#endif
			e.n_threads *= mult;
		      }
		    else
		      e.n_threads = 1;

		    // SSL
		    if (o.size() >= 5-local+n_threads_exists)
		      {
			const std::string& ssl_qualifier = o.get(4-local+n_threads_exists, 16);
			if (ssl_qualifier == "ssl")
			  {
			    if (local)
			      OPENVPN_THROW(option_error, e.directive << ": SSL not supported on local sockets");
			    e.ssl = Item::SSLOn;
			  }
			else if (ssl_qualifier == "!ssl")
			  e.ssl = Item::SSLOff;
			else
			  OPENVPN_THROW(option_error, e.directive << ": unrecognized SSL qualifier");
		      }

		    push_back(std::move(e));
		  }
	      }
	  }
	else if (load_mode == AllowDefault)
	  {
	    Item e;

	    // parse "proto" option if present
	    {
	      const Option* o = opt.get_ptr("proto");
	      if (o)
		e.proto = Protocol::parse(o->get(1, 16), Protocol::SERVER_SUFFIX);
	      else
		e.proto = Protocol(Protocol::UDPv4);
	    }

	    // parse "port" option if present
	    {
	      const Option* o = opt.get_ptr("lport");
	      if (!o)
		o = opt.get_ptr("port");
	      if (o)
		{
		  e.port = o->get(1, 16);
		  HostPort::validate_port(e.port, "listen");
		}
	      else
		e.port = "1194";
	    }

	    // parse "local" option if present
	    {
	      const Option* o = opt.get_ptr("local");
	      if (o)
		{
		  e.addr = o->get(1, 128);
		  const IP::Addr addr = IP::Addr(e.addr, "local addr");
		  e.proto.mod_addr_version(addr);
		}
	      else if (e.proto.is_ipv6())
		e.addr = "::0";
	      else
		e.addr = "0.0.0.0";
	    }

	    // n_threads defaults to one unless "listen" directive is used
	    e.n_threads = 1;

	    push_back(std::move(e));
	  }
	else if (load_mode != AllowEmpty)
	  OPENVPN_THROW(option_error, "no " << directive << " directives found");
      }

      unsigned int total_threads() const
      {
	unsigned int ret = 0;
	for (const_iterator i = begin(); i != end(); ++i)
	  ret += i->n_threads;
	return ret;
      }

    private:
      static bool match(const std::string& directive, const Option& o)
      {
	const size_t len = directive.length();
	if (len && o.size())
	  {
	    if (directive[len-1] == '-')
	      return string::starts_with(o.ref(0), directive);
	    else
	      return o.ref(0) == directive;
	  }
	else
	  return false;
      }
    };
  }
}

#endif
