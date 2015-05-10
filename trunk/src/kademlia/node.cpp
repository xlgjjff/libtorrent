/*

Copyright (c) 2006-2014, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/aux_/disable_warnings_push.hpp"

#include <utility>
#include <boost/bind.hpp>
#include <boost/function/function1.hpp>

#include "libtorrent/aux_/disable_warnings_pop.hpp"

#include "libtorrent/io.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/random.hpp"
#include "libtorrent/aux_/session_impl.hpp"
#include "libtorrent/alert_types.hpp" // for dht_lookup

#include "libtorrent/kademlia/node_id.hpp"
#include "libtorrent/kademlia/rpc_manager.hpp"
#include "libtorrent/kademlia/routing_table.hpp"
#include "libtorrent/kademlia/node.hpp"
#include "libtorrent/kademlia/dht_observer.hpp"

#include "libtorrent/kademlia/refresh.hpp"
#include "libtorrent/kademlia/get_peers.hpp"
#include "libtorrent/kademlia/get_item.hpp"
#include "libtorrent/performance_counters.hpp" // for counters

#ifdef TORRENT_USE_VALGRIND
#include <valgrind/memcheck.h>
#endif

namespace libtorrent { namespace dht
{

using detail::write_endpoint;

// TODO: 2 make this configurable in dht_settings
enum { announce_interval = 30 };

namespace {

// remove peers that have timed out
void purge_peers(std::set<peer_entry>& peers)
{
	for (std::set<peer_entry>::iterator i = peers.begin()
		  , end(peers.end()); i != end;)
	{
		// the peer has timed out
		if (i->added + minutes(int(announce_interval * 1.5f)) < aux::time_now())
			peers.erase(i++);
		else
			++i;
	}
}

void nop() {}

node_id calculate_node_id(node_id const& nid, dht_observer* observer)
{
	address external_address;
	if (observer) external_address = observer->external_address();
	if (nid == (node_id::min)() || !verify_id(nid, external_address))
		return generate_id(external_address);
  	
	return nid;
}

} // anonymous namespace

node::node(udp_socket_interface* sock
	, dht_settings const& settings, node_id nid
	, dht_observer* observer
	, struct counters& cnt)
	: m_settings(settings)
	, m_id(calculate_node_id(nid, observer))
	, m_table(m_id, 8, settings, observer)
	, m_rpc(m_id, m_table, sock, observer)
	, m_observer(observer)
	, m_last_tracker_tick(aux::time_now())
	, m_last_self_refresh(min_time())
	, m_sock(sock)
	, m_counters(cnt)
{
	m_secret[0] = random();
	m_secret[1] = random();
}

bool node::verify_token(std::string const& token, char const* info_hash
	, udp::endpoint const& addr)
{
	if (token.length() != 4)
	{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		m_observer->log(dht_logger::node, "token of incorrect length: %d"
			, token.length());
#endif
		return false;
	}

	hasher h1;
	error_code ec;
	std::string address = addr.address().to_string(ec);
	if (ec) return false;
	h1.update(&address[0], address.length());
	h1.update((char*)&m_secret[0], sizeof(m_secret[0]));
	h1.update((char*)info_hash, sha1_hash::size);
	
	sha1_hash h = h1.final();
	if (std::equal(token.begin(), token.end(), (char*)&h[0]))
		return true;
		
	hasher h2;
	h2.update(&address[0], address.length());
	h2.update((char*)&m_secret[1], sizeof(m_secret[1]));
	h2.update((char*)info_hash, sha1_hash::size);
	h = h2.final();
	if (std::equal(token.begin(), token.end(), (char*)&h[0]))
		return true;
	return false;
}

std::string node::generate_token(udp::endpoint const& addr, char const* info_hash)
{
	std::string token;
	token.resize(4);
	hasher h;
	error_code ec;
	std::string address = addr.address().to_string(ec);
	TORRENT_ASSERT(!ec);
	h.update(&address[0], address.length());
	h.update((char*)&m_secret[0], sizeof(m_secret[0]));
	h.update(info_hash, sha1_hash::size);

	sha1_hash hash = h.final();
	std::copy(hash.begin(), hash.begin() + 4, (char*)&token[0]);
	TORRENT_ASSERT(std::equal(token.begin(), token.end(), (char*)&hash[0]));
	return token;
}

void node::bootstrap(std::vector<udp::endpoint> const& nodes
	, find_data::nodes_callback const& f)
{
	node_id target = m_id;
	make_id_secret(target);

	boost::intrusive_ptr<dht::bootstrap> r(new dht::bootstrap(*this, target, f));
	m_last_self_refresh = aux::time_now();

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	int count = 0;
#endif

	for (std::vector<udp::endpoint>::const_iterator i = nodes.begin()
		, end(nodes.end()); i != end; ++i)
	{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		++count;
#endif
		r->add_entry(node_id(0), *i, observer::flag_initial);
	}
	
	// make us start as far away from our node ID as possible
	r->trim_seed_nodes();

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	m_observer->log(dht_logger::node, "bootstrapping with %d nodes", count);
#endif
	r->start();
}

int node::bucket_size(int bucket)
{
	return m_table.bucket_size(bucket);
}

void node::new_write_key()
{
	m_secret[1] = m_secret[0];
	m_secret[0] = random();
}

void node::unreachable(udp::endpoint const& ep)
{
	m_rpc.unreachable(ep);
}

void node::incoming(msg const& m)
{
	// is this a reply?
	bdecode_node y_ent = m.message.dict_find_string("y");
	if (!y_ent || y_ent.string_length() == 0)
	{
		// don't respond to this obviously broken messages. We don't
		// want to open up a magnification opportunity
//		entry e;
//		incoming_error(e, "missing 'y' entry");
//		m_sock.send_packet(e, m.addr, 0);
		return;
	}

	char y = *(y_ent.string_ptr());

	bdecode_node ext_ip = m.message.dict_find_string("ip");

	// backwards compatibility
	if (!ext_ip)
	{
		bdecode_node r = m.message.dict_find_dict("r");
		if (r)
			ext_ip = r.dict_find_string("ip");
	}

#if TORRENT_USE_IPV6
	if (ext_ip && ext_ip.string_length() >= 16)
	{
		// this node claims we use the wrong node-ID!
		address_v6::bytes_type b;
		memcpy(&b[0], ext_ip.string_ptr(), 16);
		if (m_observer)
			m_observer->set_external_address(address_v6(b)
				, m.addr.address());
	} else
#endif
	if (ext_ip && ext_ip.string_length() >= 4)
	{
		address_v4::bytes_type b;
		memcpy(&b[0], ext_ip.string_ptr(), 4);
		if (m_observer)
			m_observer->set_external_address(address_v4(b)
				, m.addr.address());
	}

	switch (y)
	{
		case 'r':
		{
			node_id id;
			m_rpc.incoming(m, &id, m_settings);
			break;
		}
		case 'q':
		{
			TORRENT_ASSERT(m.message.dict_find_string_value("y") == "q");
			entry e;
			incoming_request(m, e);
			m_sock->send_packet(e, m.addr, 0);
			break;
		}
		case 'e':
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			bdecode_node err = m.message.dict_find_list("e");
			if (err && err.list_size() >= 2)
			{
				m_observer->log(dht_logger::node, "INCOMING ERROR: %s"
					, err.list_string_value_at(1).c_str());
			}
#endif
			node_id id;
			m_rpc.incoming(m, &id, m_settings);
			break;
		}
	}
}

namespace
{
	void announce_fun(std::vector<std::pair<node_entry, std::string> > const& v
		, node& node, int listen_port, sha1_hash const& ih, int flags)
	{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		char hex_ih[41];
		to_hex(reinterpret_cast<char const*>(&ih[0]), 20, hex_ih);
		node.observer()->log(dht_logger::node, "sending announce_peer [ ih: %s "
			" p: %d nodes: %d ]", hex_ih, listen_port, int(v.size()));
#endif

		// create a dummy traversal_algorithm		
		boost::intrusive_ptr<traversal_algorithm> algo(
			new traversal_algorithm(node, (node_id::min)()));

		// store on the first k nodes
		for (std::vector<std::pair<node_entry, std::string> >::const_iterator i = v.begin()
			, end(v.end()); i != end; ++i)
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			node.observer()->log(dht_logger::node, "announce-distance: %d"
				, (160 - distance_exp(ih, i->first.id)));
#endif

			void* ptr = node.m_rpc.allocate_observer();
			if (ptr == 0) return;
			observer_ptr o(new (ptr) announce_observer(algo, i->first.ep(), i->first.id));
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			o->m_in_constructor = false;
#endif
			entry e;
			e["y"] = "q";
			e["q"] = "announce_peer";
			entry& a = e["a"];
			a["info_hash"] = ih.to_string();
			a["port"] = listen_port;
			a["token"] = i->second;
			a["seed"] = (flags & node::flag_seed) ? 1 : 0;
			if (flags & node::flag_implied_port) a["implied_port"] = 1;
			node.stats_counters().inc_stats_counter(counters::dht_announce_peer_out);
			node.m_rpc.invoke(e, i->first.ep(), o);
		}
	}
}

void node::add_router_node(udp::endpoint router)
{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	m_observer->log(dht_logger::node, "adding router node: %s"
		, print_endpoint(router).c_str());
#endif
	m_table.add_router_node(router);
}

void node::add_node(udp::endpoint node)
{
	// ping the node, and if we get a reply, it
	// will be added to the routing table
	send_single_refresh(node, m_table.num_active_buckets());
}

void node::announce(sha1_hash const& info_hash, int listen_port, int flags
	, boost::function<void(std::vector<tcp::endpoint> const&)> f)
{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	char hex_ih[41];
	to_hex(reinterpret_cast<char const*>(&info_hash[0]), 20, hex_ih);
	m_observer->log(dht_logger::node, "announcing [ ih: %s p: %d ]"
		, hex_ih, listen_port);
#endif
	// search for nodes with ids close to id or with peers
	// for info-hash id. then send announce_peer to them.

	boost::intrusive_ptr<get_peers> ta;
	if (m_settings.privacy_lookups)
	{
		ta.reset(new obfuscated_get_peers(*this, info_hash, f
			, boost::bind(&announce_fun, _1, boost::ref(*this)
			, listen_port, info_hash, flags), flags & node::flag_seed));
	}
	else
	{
		ta.reset(new get_peers(*this, info_hash, f
			, boost::bind(&announce_fun, _1, boost::ref(*this)
			, listen_port, info_hash, flags), flags & node::flag_seed));
	}

	ta->start();
}

void node::get_item(sha1_hash const& target
	, boost::function<bool(item&)> f)
{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	char hex_target[41];
	to_hex(reinterpret_cast<char const*>(&target[0]), 20, hex_target);
	m_observer->log(dht_logger::node, "starting get for [ hash: %s ]"
		, hex_target);
#endif

	boost::intrusive_ptr<dht::get_item> ta;
	ta.reset(new dht::get_item(*this, target, f));
	ta->start();
}

void node::get_item(char const* pk, std::string const& salt
	, boost::function<bool(item&)> f)
{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	char hex_key[65];
	to_hex(pk, 32, hex_key);
	m_observer->log(dht_logger::node, "starting get for [ key: %s ]", hex_key);
#endif

	boost::intrusive_ptr<dht::get_item> ta;
	ta.reset(new dht::get_item(*this, pk, salt, f));
	ta->start();
}

struct ping_observer : observer
{
	ping_observer(
		boost::intrusive_ptr<traversal_algorithm> const& algorithm
		, udp::endpoint const& ep, node_id const& id)
		: observer(algorithm, ep, id)
	{}

	// parses out "nodes"
	virtual void reply(msg const& m)
	{
		flags |= flag_done;

		bdecode_node r = m.message.dict_find_dict("r");
		if (!r)
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			m_algorithm->get_node().observer()->log(dht_logger::node
				, "[%p] missing response dict"
				, m_algorithm.get());
#endif
			return;
		}

		// look for nodes
		bdecode_node n = r.dict_find_string("nodes");
		if (n)
		{
			char const* nodes = n.string_ptr();
			char const* end = nodes + n.string_length();

			while (end - nodes >= 26)
			{
				node_id id;
				std::copy(nodes, nodes + 20, id.begin());
				nodes += 20;
				m_algorithm.get()->get_node().m_table.heard_about(id
					, detail::read_v4_endpoint<udp::endpoint>(nodes));
			}
		}
	}
};


void node::tick()
{
	// every now and then we refresh our own ID, just to keep
	// expanding the routing table buckets closer to us.
	time_point now = aux::time_now();
	if (m_last_self_refresh + minutes(10) < now)
	{
		node_id target = m_id;
		make_id_secret(target);
		boost::intrusive_ptr<dht::bootstrap> r(new dht::bootstrap(*this, target
			, boost::bind(&nop)));
		r->start();
		m_last_self_refresh = now;
		return;
	}

	node_entry const* ne = m_table.next_refresh();
	if (ne == NULL) return;

	// this shouldn't happen
	TORRENT_ASSERT(m_id != ne->id);
	if (ne->id == m_id) return;

	int bucket = 159 - distance_exp(m_id, ne->id);
	TORRENT_ASSERT(bucket < 160);
	send_single_refresh(ne->ep(), bucket, ne->id);
}

void node::send_single_refresh(udp::endpoint const& ep, int bucket
	, node_id const& id)
{
	TORRENT_ASSERT(id != m_id);
	void* ptr = m_rpc.allocate_observer();
	if (ptr == 0) return;

	TORRENT_ASSERT(bucket >= 0);
	TORRENT_ASSERT(bucket <= 159);

	// generate a random node_id within the given bucket
	// TODO: 2 it would be nice to have a bias towards node-id prefixes that
	// are missing in the bucket
	node_id mask = generate_prefix_mask(bucket + 1);
	node_id target = generate_secret_id() & ~mask;
	target |= m_id & mask;

	// create a dummy traversal_algorithm		
	// this is unfortunately necessary for the observer
	// to free itself from the pool when it's being released
	boost::intrusive_ptr<traversal_algorithm> algo(
		new traversal_algorithm(*this, (node_id::min)()));
	observer_ptr o(new (ptr) ping_observer(algo, ep, id));
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	o->m_in_constructor = false;
#endif
	entry e;
	e["y"] = "q";
	entry& a = e["a"];

	// use get_peers instead of find_node. We'll get nodes in the response
	// either way.
	e["q"] = "get_peers";
	a["info_hash"] = target.to_string();
	m_counters.inc_stats_counter(counters::dht_get_peers_out);

//	e["q"] = "find_node";
//	a["target"] = target.to_string();
	m_rpc.invoke(e, ep, o);
}

time_duration node::connection_timeout()
{
	time_duration d = m_rpc.tick();
	time_point now(aux::time_now());
	if (now - minutes(2) < m_last_tracker_tick) return d;
	m_last_tracker_tick = now;

	for (dht_immutable_table_t::iterator i = m_immutable_table.begin();
		i != m_immutable_table.end();)
	{
		if (i->second.last_seen + minutes(60) > now)
		{
			++i;
			continue;
		}
		free(i->second.value);
		m_immutable_table.erase(i++);
		m_counters.inc_stats_counter(counters::dht_immutable_data, -1);
	}

	// look through all peers and see if any have timed out
	for (table_t::iterator i = m_map.begin(), end(m_map.end()); i != end;)
	{
		torrent_entry& t = i->second;
		node_id const& key = i->first;
		++i;
		purge_peers(t.peers);

		// if there are no more peers, remove the entry altogether
		if (t.peers.empty())
		{
			table_t::iterator i = m_map.find(key);
			if (i != m_map.end())
			{
				m_map.erase(i);
				m_counters.inc_stats_counter(counters::dht_torrents, -1);
			}
		}
	}

	return d;
}

void node::status(std::vector<dht_routing_bucket>& table
	, std::vector<dht_lookup>& requests)
{
	mutex_t::scoped_lock l(m_mutex);

	m_table.status(table);

	for (std::set<traversal_algorithm*>::iterator i = m_running_requests.begin()
		, end(m_running_requests.end()); i != end; ++i)
	{
		requests.push_back(dht_lookup());
		dht_lookup& l = requests.back();
		(*i)->status(l);
	}
}

#ifndef TORRENT_NO_DEPRECATE
// TODO: 2 use the non deprecated function instead of this one
void node::status(session_status& s)
{
	mutex_t::scoped_lock l(m_mutex);

	m_table.status(s);
	s.dht_torrents = int(m_map.size());
	s.active_requests.clear();
	s.dht_total_allocations = m_rpc.num_allocated_observers();
	for (std::set<traversal_algorithm*>::iterator i = m_running_requests.begin()
		, end(m_running_requests.end()); i != end; ++i)
	{
		s.active_requests.push_back(dht_lookup());
		dht_lookup& l = s.active_requests.back();
		(*i)->status(l);
	}
}
#endif

void node::lookup_peers(sha1_hash const& info_hash, entry& reply
	, bool noseed, bool scrape) const
{
	if (m_observer)
		m_observer->get_peers(info_hash);

	table_t::const_iterator i = m_map.lower_bound(info_hash);
	if (i == m_map.end()) return;
	if (i->first != info_hash) return;

	torrent_entry const& v = i->second;

	if (!v.name.empty()) reply["n"] = v.name;

	if (scrape)
	{
		bloom_filter<256> downloaders;
		bloom_filter<256> seeds;

		for (std::set<peer_entry>::const_iterator i = v.peers.begin()
			, end(v.peers.end()); i != end; ++i)
		{
			sha1_hash iphash;
			hash_address(i->addr.address(), iphash);
			if (i->seed) seeds.set(iphash);
			else downloaders.set(iphash);
		}

		reply["BFpe"] = downloaders.to_string();
		reply["BFsd"] = seeds.to_string();
	}
	else
	{
		int num = (std::min)((int)v.peers.size(), m_settings.max_peers_reply);
		std::set<peer_entry>::const_iterator iter = v.peers.begin();
		entry::list_type& pe = reply["values"].list();
		std::string endpoint;

		for (int t = 0, m = 0; m < num && iter != v.peers.end(); ++iter, ++t)
		{
			if ((random() / float(UINT_MAX + 1.f)) * (num - t) >= num - m) continue;
			if (noseed && iter->seed) continue;
			endpoint.resize(18);
			std::string::iterator out = endpoint.begin();
			write_endpoint(iter->addr, out);
			endpoint.resize(out - endpoint.begin());
			pe.push_back(entry(endpoint));

			++m;
		}
	}
	return;
}

void TORRENT_EXTRA_EXPORT write_nodes_entry(entry& r, nodes_t const& nodes)
{
	entry& n = r["nodes"];
	std::back_insert_iterator<std::string> out(n.string());
	for (nodes_t::const_iterator i = nodes.begin()
		, end(nodes.end()); i != end; ++i)
	{
		if (!i->addr().is_v4()) continue;
		std::copy(i->id.begin(), i->id.end(), out);
		write_endpoint(udp::endpoint(i->addr(), i->port()), out);
	}
}

// verifies that a message has all the required
// entries and returns them in ret
bool verify_message(bdecode_node const& message, key_desc_t const desc[]
	, bdecode_node ret[], int size , char* error, int error_size)
{
	// get a non-root bdecode_node that still
	// points to the root. message should not be copied
	bdecode_node msg = message.non_owning();

	// clear the return buffer
	for (int i = 0; i < size; ++i)
		ret[i].clear();

	// when parsing child nodes, this is the stack
	// of bdecode_nodes to return to
	bdecode_node stack[5];
	int stack_ptr = -1;

	if (msg.type() != bdecode_node::dict_t)
	{
		snprintf(error, error_size, "not a dictionary");
		return false;
	}
	++stack_ptr;
	stack[stack_ptr] = msg;
	for (int i = 0; i < size; ++i)
	{
		key_desc_t const& k = desc[i];

//		fprintf(stderr, "looking for %s in %s\n", k.name, print_entry(*msg).c_str());

		ret[i] = msg.dict_find(k.name);
		// none_t means any type
		if (ret[i] && ret[i].type() != k.type && k.type != bdecode_node::none_t)
			ret[i].clear();
		if (ret[i] == 0 && (k.flags & key_desc_t::optional) == 0)
		{
			// the key was not found, and it's not an optional key
			snprintf(error, error_size, "missing '%s' key", k.name);
			return false;
		}

		if (k.size > 0
			&& ret[i]
			&& k.type == bdecode_node::string_t)
		{
			bool invalid = false;
			if (k.flags & key_desc_t::size_divisible)
				invalid = (ret[i].string_length() % k.size) != 0;
			else
				invalid = ret[i].string_length() != k.size;

			if (invalid)
			{
				// the string was not of the required size
				ret[i].clear();
				if ((k.flags & key_desc_t::optional) == 0)
				{
					snprintf(error, error_size, "invalid value for '%s'", k.name);
					return false;
				}
			}
		}
		if (k.flags & key_desc_t::parse_children)
		{
			TORRENT_ASSERT(k.type == bdecode_node::dict_t);

			if (ret[i])
			{
				++stack_ptr;
				TORRENT_ASSERT(stack_ptr < int(sizeof(stack)/sizeof(stack[0])));
				msg = ret[i];
				stack[stack_ptr] = msg;
			}
			else
			{
				// skip all children
				while (i < size && (desc[i].flags & key_desc_t::last_child) == 0) ++i;
				// if this assert is hit, desc is incorrect
				TORRENT_ASSERT(i < size);
			}
		}
		else if (k.flags & key_desc_t::last_child)
		{
			TORRENT_ASSERT(stack_ptr > 0);
			// this can happen if the specification passed
			// in is unbalanced. i.e. contain more last_child
			// nodes than parse_children
			if (stack_ptr == 0) return false;
			--stack_ptr;
			msg = stack[stack_ptr];
		}
	}
	return true;
}

void incoming_error(entry& e, char const* msg, int error_code)
{
	e["y"] = "e";
	entry::list_type& l = e["e"].list();
	l.push_back(entry(error_code));
	l.push_back(entry(msg));
}

// return true of the first argument is a better canidate for removal, i.e.
// less important to keep
struct immutable_item_comparator
{
	immutable_item_comparator(node_id const& our_id) : m_our_id(our_id) {}

	bool operator() (std::pair<node_id, dht_immutable_item> const& lhs
		, std::pair<node_id, dht_immutable_item> const& rhs) const
	{
		int l_distance = distance_exp(lhs.first, m_our_id);
		int r_distance = distance_exp(rhs.first, m_our_id);

		// this is a score taking the popularity (number of announcers) and the
		// fit, in terms of distance from ideal storing node, into account.
		// each additional 5 announcers is worth one extra bit in the distance.
		// that is, an item with 10 announcers is allowed to be twice as far
		// from another item with 5 announcers, from our node ID. Twice as far
		// because it gets one more bit.
		return lhs.second.num_announcers / 5 - l_distance < rhs.second.num_announcers / 5 - r_distance;
	}

private:
	// explicitly disallow assignment, to silence msvc warning
	immutable_item_comparator& operator=(immutable_item_comparator const&);

	node_id const& m_our_id;
};

// build response
void node::incoming_request(msg const& m, entry& e)
{
	if (!m_sock->has_quota())
		return;

	e = entry(entry::dictionary_t);
	e["y"] = "r";
	e["t"] = m.message.dict_find_string_value("t");

	key_desc_t top_desc[] = {
		{"q", bdecode_node::string_t, 0, 0},
		{"ro", bdecode_node::int_t, 0, key_desc_t::optional},
		{"a", bdecode_node::dict_t, 0, key_desc_t::parse_children},
			{"id", bdecode_node::string_t, 20, key_desc_t::last_child},
	};

	bdecode_node top_level[4];
	char error_string[200];
	if (!verify_message(m.message, top_desc, top_level, 4, error_string
		, sizeof(error_string)))
	{
		incoming_error(e, error_string);
		return;
	}

	e["ip"] = endpoint_to_bytes(m.addr);

	char const* query = top_level[0].string_ptr();
	int query_len = top_level[0].string_length();

	bdecode_node arg_ent = top_level[2];
	bool read_only = top_level[1] && top_level[1].int_value() != 0;
	node_id id(top_level[3].string_ptr());

	// if this nodes ID doesn't match its IP, tell it what
	// its IP is with an error
	// don't enforce this yet
	if (m_settings.enforce_node_id && !verify_id(id, m.addr.address()))
	{
		incoming_error(e, "invalid node ID");
		return;
	}

	if (!read_only)
		m_table.heard_about(id, m.addr);

	entry& reply = e["r"];
	m_rpc.add_our_id(reply);

	// mirror back the other node's external port
	reply["p"] = m.addr.port();

	if (query_len == 4 && memcmp(query, "ping", 4) == 0)
	{
		m_counters.inc_stats_counter(counters::dht_ping_in);
		// we already have 't' and 'id' in the response
		// no more left to add
	}
	else if (query_len == 9 && memcmp(query, "get_peers", 9) == 0)
	{
		key_desc_t msg_desc[] = {
			{"info_hash", bdecode_node::string_t, 20, 0},
			{"noseed", bdecode_node::int_t, 0, key_desc_t::optional},
			{"scrape", bdecode_node::int_t, 0, key_desc_t::optional},
		};

		bdecode_node msg_keys[3];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 3, error_string
			, sizeof(error_string)))
		{
			m_counters.inc_stats_counter(counters::dht_invalid_get_peers);
			incoming_error(e, error_string);
			return;
		}

		reply["token"] = generate_token(m.addr, msg_keys[0].string_ptr());
		
		m_counters.inc_stats_counter(counters::dht_get_peers_in);

		sha1_hash info_hash(msg_keys[0].string_ptr());
		nodes_t n;
		// always return nodes as well as peers
		m_table.find_node(info_hash, n, 0);
		write_nodes_entry(reply, n);

		bool noseed = false;
		bool scrape = false;
		if (msg_keys[1] && msg_keys[1].int_value() != 0) noseed = true;
		if (msg_keys[2] && msg_keys[2].int_value() != 0) scrape = true;
		lookup_peers(info_hash, reply, noseed, scrape);
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		if (reply.find_key("values"))
		{
			m_observer->log(dht_logger::node, "values: %d"
				, int(reply["values"].list().size()));
		}
#endif
	}
	else if (query_len == 9 && memcmp(query, "find_node", 9) == 0)
	{
		key_desc_t msg_desc[] = {
			{"target", bdecode_node::string_t, 20, 0},
		};

		bdecode_node msg_keys[1];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 1, error_string, sizeof(error_string)))
		{
			incoming_error(e, error_string);
			return;
		}

		m_counters.inc_stats_counter(counters::dht_find_node_in);
		sha1_hash target(msg_keys[0].string_ptr());

		// TODO: 2 find_node should write directly to the response entry
		nodes_t n;
		m_table.find_node(target, n, 0);
		write_nodes_entry(reply, n);
	}
	else if (query_len == 13 && memcmp(query, "announce_peer", 13) == 0)
	{
		key_desc_t msg_desc[] = {
			{"info_hash", bdecode_node::string_t, 20, 0},
			{"port", bdecode_node::int_t, 0, 0},
			{"token", bdecode_node::string_t, 0, 0},
			{"n", bdecode_node::string_t, 0, key_desc_t::optional},
			{"seed", bdecode_node::int_t, 0, key_desc_t::optional},
			{"implied_port", bdecode_node::int_t, 0, key_desc_t::optional},
		};

		bdecode_node msg_keys[6];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 6, error_string, sizeof(error_string)))
		{
			m_counters.inc_stats_counter(counters::dht_invalid_announce);
			incoming_error(e, error_string);
			return;
		}

		int port = int(msg_keys[1].int_value());

		// is the announcer asking to ignore the explicit
		// listen port and instead use the source port of the packet?
		if (msg_keys[5] && msg_keys[5].int_value() != 0)
			port = m.addr.port();

		if (port < 0 || port >= 65536)
		{
			m_counters.inc_stats_counter(counters::dht_invalid_announce);
			incoming_error(e, "invalid port");
			return;
		}

		sha1_hash info_hash(msg_keys[0].string_ptr());

		if (m_observer)
			m_observer->announce(info_hash, m.addr.address(), port);

		if (!verify_token(msg_keys[2].string_value(), msg_keys[0].string_ptr(), m.addr))
		{
			m_counters.inc_stats_counter(counters::dht_invalid_announce);
			incoming_error(e, "invalid token");
			return;
		}

		m_counters.inc_stats_counter(counters::dht_announce_peer_in);

		// the token was correct. That means this
		// node is not spoofing its address. So, let
		// the table get a chance to add it.
		m_table.node_seen(id, m.addr, 0xffff);

		table_t::iterator ti = m_map.find(info_hash);
		torrent_entry* v;
		if (ti == m_map.end())
		{
			// we don't have this torrent, add it
			// do we need to remove another one first?
			if (!m_map.empty() && int(m_map.size()) >= m_settings.max_torrents)
			{
				// we need to remove some. Remove the ones with the
				// fewest peers
				int num_peers = m_map.begin()->second.peers.size();
				table_t::iterator candidate = m_map.begin();
				for (table_t::iterator i = m_map.begin()
					, end(m_map.end()); i != end; ++i)
				{
					if (int(i->second.peers.size()) > num_peers) continue;
					if (i->first == info_hash) continue;
					num_peers = i->second.peers.size();
					candidate = i;
				}
				m_map.erase(candidate);
				m_counters.inc_stats_counter(counters::dht_torrents, -1);
			}
			m_counters.inc_stats_counter(counters::dht_torrents);
	  		v = &m_map[info_hash];
		}
		else
		{
			v = &ti->second;
		}

		// the peer announces a torrent name, and we don't have a name
		// for this torrent. Store it.
		if (msg_keys[3] && v->name.empty())
		{
			std::string name = msg_keys[3].string_value();
			if (name.size() > 50) name.resize(50);
			v->name = name;
		}

		peer_entry peer;
		peer.addr = tcp::endpoint(m.addr.address(), port);
		peer.added = aux::time_now();
		peer.seed = msg_keys[4] && msg_keys[4].int_value();
		std::set<peer_entry>::iterator i = v->peers.find(peer);
		if (i != v->peers.end()) v->peers.erase(i++);
		v->peers.insert(i, peer);
	}
	else if (query_len == 3 && memcmp(query, "put", 3) == 0)
	{
		// the first 2 entries are for both mutable and
		// immutable puts
		static const key_desc_t msg_desc[] = {
			{"token", bdecode_node::string_t, 0, 0},
			{"v", bdecode_node::none_t, 0, 0},
			{"seq", bdecode_node::int_t, 0, key_desc_t::optional},
			// public key
			{"k", bdecode_node::string_t, item_pk_len, key_desc_t::optional},
			{"sig", bdecode_node::string_t, item_sig_len, key_desc_t::optional},
			{"cas", bdecode_node::int_t, 0, key_desc_t::optional},
			{"salt", bdecode_node::string_t, 0, key_desc_t::optional},
		};

		// attempt to parse the message
		bdecode_node msg_keys[7];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 7, error_string, sizeof(error_string)))
		{
			m_counters.inc_stats_counter(counters::dht_invalid_put);
			incoming_error(e, error_string);
			return;
		}

		m_counters.inc_stats_counter(counters::dht_put_in);

		// is this a mutable put?
		bool mutable_put = (msg_keys[2] && msg_keys[3] && msg_keys[4]);

		// public key (only set if it's a mutable put)
		char const* pk = NULL;
		if (msg_keys[3]) pk = msg_keys[3].string_ptr();

		// signature (only set if it's a mutable put)
		char const* sig = NULL;
		if (msg_keys[4]) sig = msg_keys[4].string_ptr();

		// pointer and length to the whole entry
		std::pair<char const*, int> buf = msg_keys[1].data_section();
		if (buf.second > 1000 || buf.second <= 0)
		{
			m_counters.inc_stats_counter(counters::dht_invalid_put);
			incoming_error(e, "message too big", 205);
			return;
		}

		std::pair<char const*, int> salt(static_cast<char const*>(NULL), 0);
		if (msg_keys[6])
			salt = std::pair<char const*, int>(
				msg_keys[6].string_ptr(), msg_keys[6].string_length());
		if (salt.second > 64)
		{
			m_counters.inc_stats_counter(counters::dht_invalid_put);
			incoming_error(e, "salt too big", 207);
			return;
		}

		sha1_hash target;
		if (pk)
			target = item_target_id(salt, pk);
		else
			target = item_target_id(buf);

//		fprintf(stderr, "%s PUT target: %s salt: %s key: %s\n"
//			, mutable_put ? "mutable":"immutable"
//			, to_hex(target.to_string()).c_str()
//			, salt.second > 0 ? std::string(salt.first, salt.second).c_str() : ""
//			, pk ? to_hex(std::string(pk, 32)).c_str() : "");

		// verify the write-token. tokens are only valid to write to
		// specific target hashes. it must match the one we got a "get" for
		if (!verify_token(msg_keys[0].string_value(), (char const*)&target[0], m.addr))
		{
			m_counters.inc_stats_counter(counters::dht_invalid_put);
			incoming_error(e, "invalid token");
			return;
		}

		dht_immutable_item* f = 0;

		if (!mutable_put)
		{
			dht_immutable_table_t::iterator i = m_immutable_table.find(target);
			if (i == m_immutable_table.end())
			{
				// make sure we don't add too many items
				if (int(m_immutable_table.size()) >= m_settings.max_dht_items)
				{
					// delete the least important one (i.e. the one
					// the fewest peers are announcing, and farthest
					// from our node ID)
					dht_immutable_table_t::iterator j = std::min_element(m_immutable_table.begin()
						, m_immutable_table.end()
						, immutable_item_comparator(m_id));

					TORRENT_ASSERT(j != m_immutable_table.end());
					free(j->second.value);
					m_immutable_table.erase(j);
					m_counters.inc_stats_counter(counters::dht_immutable_data, -1);
				}
				dht_immutable_item to_add;
				to_add.value = (char*)malloc(buf.second);
				to_add.size = buf.second;
				memcpy(to_add.value, buf.first, buf.second);
		
				boost::tie(i, boost::tuples::ignore) = m_immutable_table.insert(
					std::make_pair(target, to_add));
				m_counters.inc_stats_counter(counters::dht_immutable_data);
			}

//			fprintf(stderr, "added immutable item (%d)\n", int(m_immutable_table.size()));

			f = &i->second;
		}
		else
		{
			// mutable put, we must verify the signature

#ifdef TORRENT_USE_VALGRIND
			VALGRIND_CHECK_MEM_IS_DEFINED(msg_keys[4].string_ptr(), item_sig_len);
			VALGRIND_CHECK_MEM_IS_DEFINED(pk, item_pk_len);
#endif
			// msg_keys[4] is the signature, msg_keys[3] is the public key
			if (!verify_mutable_item(buf, salt
				, msg_keys[2].int_value(), pk, sig))
			{
				m_counters.inc_stats_counter(counters::dht_invalid_put);
				incoming_error(e, "invalid signature", 206);
				return;
			}

			dht_mutable_table_t::iterator i = m_mutable_table.find(target);
			if (i == m_mutable_table.end())
			{
				// this is the case where we don't have an item in this slot
				// make sure we don't add too many items
				if (int(m_mutable_table.size()) >= m_settings.max_dht_items)
				{
					// delete the least important one (i.e. the one
					// the fewest peers are announcing)
					dht_mutable_table_t::iterator j = std::min_element(m_mutable_table.begin()
						, m_mutable_table.end()
						, boost::bind(&dht_immutable_item::num_announcers
							, boost::bind(&dht_mutable_table_t::value_type::second, _1)));
					TORRENT_ASSERT(j != m_mutable_table.end());
					free(j->second.value);
					free(j->second.salt);
					m_mutable_table.erase(j);
					m_counters.inc_stats_counter(counters::dht_mutable_data, -1);
				}
				dht_mutable_item to_add;
				to_add.value = (char*)malloc(buf.second);
				to_add.size = buf.second;
				to_add.seq = msg_keys[2].int_value();
				to_add.salt = NULL;
				to_add.salt_size = 0;
				if (salt.second > 0)
				{
					to_add.salt = (char*)malloc(salt.second);
					to_add.salt_size = salt.second;
					memcpy(to_add.salt, salt.first, salt.second);
				}
				memcpy(to_add.sig, sig, sizeof(to_add.sig));
				TORRENT_ASSERT(sizeof(to_add.sig) == msg_keys[4].string_length());
				memcpy(to_add.value, buf.first, buf.second);
				memcpy(&to_add.key, pk, sizeof(to_add.key));
		
				boost::tie(i, boost::tuples::ignore) = m_mutable_table.insert(
					std::make_pair(target, to_add));
				m_counters.inc_stats_counter(counters::dht_mutable_data);

//				fprintf(stderr, "added mutable item (%d)\n", int(m_mutable_table.size()));
			}
			else
			{
				// this is the case where we already 
				dht_mutable_item* item = &i->second;

				// this is the "cas" field in the put message
				// if it was specified, we MUST make sure the current sequence
				// number matches the expected value before replacing it
				// this is critical for avoiding race conditions when multiple
				// writers are accessing the same slot
				if (msg_keys[5] && item->seq != msg_keys[5].int_value())
				{
					m_counters.inc_stats_counter(counters::dht_invalid_put);
					incoming_error(e, "CAS mismatch", 301);
					return;
				}

				if (item->seq > boost::uint64_t(msg_keys[2].int_value()))
				{
					m_counters.inc_stats_counter(counters::dht_invalid_put);
					incoming_error(e, "old sequence number", 302);
					return;
				}

				if (item->seq < boost::uint64_t(msg_keys[2].int_value()))
				{
					if (item->size != buf.second)
					{
						free(item->value);
						item->value = (char*)malloc(buf.second);
						item->size = buf.second;
					}
					item->seq = msg_keys[2].int_value();
					memcpy(item->sig, msg_keys[4].string_ptr(), sizeof(item->sig));
					TORRENT_ASSERT(sizeof(item->sig) == msg_keys[4].string_length());
					memcpy(item->value, buf.first, buf.second);
				}
			}

			f = &i->second;
		}

		m_table.node_seen(id, m.addr, 0xffff);

		f->last_seen = aux::time_now();

		// maybe increase num_announcers if we haven't seen this IP before
		sha1_hash iphash;
		hash_address(m.addr.address(), iphash);
		if (!f->ips.find(iphash))
		{
			f->ips.set(iphash);
			++f->num_announcers;
		}
	}
	else if (query_len == 3 && memcmp(query, "get", 3) == 0)
	{
		key_desc_t msg_desc[] = {
			{"seq", bdecode_node::int_t, 0, key_desc_t::optional},
			{"target", bdecode_node::string_t, 20, 0},
		};

		// k is not used for now

		// attempt to parse the message
		bdecode_node msg_keys[2];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 2, error_string
			, sizeof(error_string)))
		{
			m_counters.inc_stats_counter(counters::dht_invalid_get);
			incoming_error(e, error_string);
			return;
		}

		m_counters.inc_stats_counter(counters::dht_get_in);
		sha1_hash target(msg_keys[1].string_ptr());

//		fprintf(stderr, "%s GET target: %s\n"
//			, msg_keys[1] ? "mutable":"immutable"
//			, to_hex(target.to_string()).c_str());

		reply["token"] = generate_token(m.addr, msg_keys[1].string_ptr());
		
		nodes_t n;
		// always return nodes as well as peers
		m_table.find_node(target, n, 0);
		write_nodes_entry(reply, n);

		dht_immutable_table_t::iterator i = m_immutable_table.end();

		// if the get has a sequence number it must be for a mutable item
		// so don't bother searching the immutable table
		if (!msg_keys[0])
			i = m_immutable_table.find(target);

		if (i != m_immutable_table.end())
		{
			dht_immutable_item const& f = i->second;
			reply["v"] = bdecode(f.value, f.value + f.size);
		}
		else
		{
			dht_mutable_table_t::iterator i = m_mutable_table.find(target);
			if (i != m_mutable_table.end())
			{
				dht_mutable_item const& f = i->second;
				reply["seq"] = f.seq;
				if (!msg_keys[0] || boost::uint64_t(msg_keys[0].int_value()) < f.seq)
				{
					reply["v"] = bdecode(f.value, f.value + f.size);
					reply["sig"] = std::string(f.sig, f.sig + sizeof(f.sig));
					reply["k"] = std::string(f.key.bytes, f.key.bytes + sizeof(f.key.bytes));
				}
			}
		}
	}
	else
	{
		// if we don't recognize the message but there's a
		// 'target' or 'info_hash' in the arguments, treat it
		// as find_node to be future compatible
		bdecode_node target_ent = arg_ent.dict_find_string("target");
		if (!target_ent || target_ent.string_length() != 20)
		{
			target_ent = arg_ent.dict_find_string("info_hash");
			if (!target_ent || target_ent.string_length() != 20)
			{
				incoming_error(e, "unknown message");
				return;
			}
		}

		sha1_hash target(target_ent.string_ptr());
		nodes_t n;
		// always return nodes as well as peers
		m_table.find_node(target, n, 0);
		write_nodes_entry(reply, n);
		return;
	}
}


} } // namespace libtorrent::dht

