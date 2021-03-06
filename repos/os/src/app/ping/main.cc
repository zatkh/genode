/*
 * \brief  Test the reachability of a host on an IP network
 * \author Martin Stein
 * \date   2018-03-27
 */

/*
 * Copyright (C) 2018 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* local includes */
#include <size_guard.h>

/* Genode includes */
#include <net/ipv4.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <base/component.h>
#include <base/heap.h>
#include <base/attached_rom_dataspace.h>
#include <timer_session/connection.h>
#include <nic_session/connection.h>
#include <nic/packet_allocator.h>

using namespace Net;
using namespace Genode;

namespace Net {

	using Packet_descriptor    = ::Nic::Packet_descriptor;
	using Packet_stream_sink   = ::Nic::Packet_stream_sink< ::Nic::Session::Policy>;
	using Packet_stream_source = ::Nic::Packet_stream_source< ::Nic::Session::Policy>;
}

Microseconds read_sec_attr(Xml_node      const  node,
                           char          const *name,
                           unsigned long const  default_sec)
{
	unsigned long sec = node.attribute_value(name, 0UL);
	if (!sec) {
		sec = default_sec;
	}
	return Microseconds(sec * 1000 * 1000);
}


class Main
{
	private:

		using Signal_handler   = Genode::Signal_handler<Main>;
		using Periodic_timeout = Timer::Periodic_timeout<Main>;

		enum { IPV4_TIME_TO_LIVE    = 64 };
		enum { ICMP_ID              = 1166 };
		enum { DEFAULT_ICMP_DATA_SZ = 56 };
		enum { DEFAULT_COUNT        = 5 };
		enum { DEFAULT_PERIOD_SEC   = 5 };
		enum { PKT_SIZE             = Nic::Packet_allocator::DEFAULT_PACKET_SIZE };
		enum { BUF_SIZE             = Nic::Session::QUEUE_SIZE * PKT_SIZE };

		Env                    &_env;
		Attached_rom_dataspace  _config_rom    { _env, "config" };
		Xml_node                _config        { _config_rom.xml() };
		Timer::Connection       _timer         { _env };
		Microseconds            _send_time     { 0 };
		Periodic_timeout        _period        { _timer, *this, &Main::_send_ping,
		                                         read_sec_attr(_config, "period_sec", DEFAULT_PERIOD_SEC) };
		Heap                    _heap          { &_env.ram(), &_env.rm() };
		Nic::Packet_allocator   _pkt_alloc     { &_heap };
		Nic::Connection         _nic           { _env, &_pkt_alloc, BUF_SIZE, BUF_SIZE };
		Signal_handler          _sink_ack      { _env.ep(), *this, &Main::_ack_avail };
		Signal_handler          _sink_submit   { _env.ep(), *this, &Main::_ready_to_submit };
		Signal_handler          _source_ack    { _env.ep(), *this, &Main::_ready_to_ack };
		Signal_handler          _source_submit { _env.ep(), *this, &Main::_packet_avail };
		bool             const  _verbose       { _config.attribute_value("verbose", false) };
		Ipv4_address     const  _src_ip        { _config.attribute_value("src_ip",  Ipv4_address()) };
		Ipv4_address     const  _dst_ip        { _config.attribute_value("dst_ip",  Ipv4_address()) };
		Mac_address      const  _src_mac       { _nic.mac_address() };
		Mac_address             _dst_mac       { };
		uint16_t                _ip_id         { 1 };
		uint16_t                _icmp_seq      { 1 };
		size_t           const  _icmp_data_sz  { _config.attribute_value("data_size", (size_t)DEFAULT_ICMP_DATA_SZ) };
		unsigned long           _count         { _config.attribute_value("count", (unsigned long)DEFAULT_COUNT) };

		void _handle_eth(void              *const  eth_base,
		                 size_t             const  eth_size,
		                 Packet_descriptor  const &pkt);

		void _handle_ip(Ethernet_frame &eth,
		                size_t const    eth_size);

		void _handle_icmp(Ipv4_packet  &ip,
		                  size_t const  ip_size);

		void _handle_icmp_echo_reply(Ipv4_packet &ip,
		                             Icmp_packet &icmp,
		                             size_t       icmp_data_sz);

		void _handle_icmp_dst_unreachbl(Ipv4_packet &ip,
		                                Icmp_packet &icmp,
		                                size_t       icmp_data_sz);

		void _handle_arp(Ethernet_frame &eth,
		                 size_t   const  eth_size);

		void _broadcast_arp_request();

		void _send_arp_reply(Ethernet_frame &req_eth,
		                     Arp_packet     &req_arp);

		template <typename FUNC>
		void _send(size_t  pkt_size,
		           FUNC && write_to_pkt)
		{
			try {
				Packet_descriptor  pkt      = _source().alloc_packet(pkt_size);
				void              *pkt_base = _source().packet_content(pkt);
				write_to_pkt(pkt_base);
				_source().submit_packet(pkt);
				if (_verbose) {
					log("snd ", *reinterpret_cast<Ethernet_frame *>(pkt_base)); }
			}
			catch (Net::Packet_stream_source::Packet_alloc_failed) {
				warning("failed to allocate packet"); }
		}

		void _send_ping(Duration not_used = Duration(Microseconds(0)));

		Net::Packet_stream_sink   &_sink()   { return *_nic.rx(); }
		Net::Packet_stream_source &_source() { return *_nic.tx(); }


		/***********************************
		 ** Packet-stream signal handlers **
		 ***********************************/

		void _ready_to_submit();
		void _ack_avail() { }
		void _ready_to_ack();
		void _packet_avail() { }

	public:

		struct Invalid_arguments     : Exception { };
		struct Send_buffer_too_small : Exception { };

		Main(Env &env);
};


Main::Main(Env &env) : _env(env)
{
	/* exit unsuccessful if parameters are invalid */
	if (_src_ip == Ipv4_address() ||
	    _dst_ip == Ipv4_address() ||
	    _count  == 0)
	{
		throw Invalid_arguments();
	}
	/* install packet stream signals */
	_nic.rx_channel()->sigh_ready_to_ack(_sink_ack);
	_nic.rx_channel()->sigh_packet_avail(_sink_submit);
	_nic.tx_channel()->sigh_ack_avail(_source_ack);
	_nic.tx_channel()->sigh_ready_to_submit(_source_submit);
}


void Main::_handle_eth(void              *const eth_base,
                       size_t             const eth_size,
                       Packet_descriptor  const &)
{
	/* print receipt message */
	Ethernet_frame &eth = *reinterpret_cast<Ethernet_frame *>(eth_base);
	if (_verbose) {
		log("rcv ", eth); }

	/* drop packet if ETH does not target us */
	if (eth.dst() != _src_mac &&
	    eth.dst() != Ethernet_frame::broadcast())
	{
		if (_verbose) {
			log("bad ETH destination"); }
		return;
	}
	/* select ETH sub-protocol */
	switch (eth.type()) {
	case Ethernet_frame::Type::ARP:  _handle_arp(eth, eth_size); break;
	case Ethernet_frame::Type::IPV4: _handle_ip(eth, eth_size);  break;
	default: ; }
}


void Main::_handle_ip(Ethernet_frame &eth,
                      size_t const    eth_size)
{
	/* drop packet if IP does not target us */
	size_t const ip_size = eth_size - sizeof(Ethernet_frame);
	Ipv4_packet &ip = *eth.data<Ipv4_packet>(ip_size);
	if (ip.dst() != _src_ip &&
	    ip.dst() != Ipv4_packet::broadcast())
	{
		if (_verbose) {
			log("bad IP destination"); }
		return;
	}
	/* drop packet if IP checksum is invalid */
	if (Ipv4_packet::calculate_checksum(ip) != ip.checksum()) {
		if (_verbose) {
			log("bad IP checksum"); }
		return;
	}
	/* select IP sub-protocol */
	switch (ip.protocol()) {
	case Ipv4_packet::Protocol::ICMP: _handle_icmp(ip, ip_size);
	default: ; }
}


void Main::_handle_icmp_echo_reply(Ipv4_packet &ip,
                                   Icmp_packet &icmp,
                                   size_t       icmp_data_sz)
{
	/* check IP source */
	if (ip.src() != _dst_ip) {
		if (_verbose) {
			log("bad IP source"); }
		return;
	}
	/* check ICMP code */
	if (icmp.code() != Icmp_packet::Code::ECHO_REPLY) {
		if (_verbose) {
			log("bad ICMP type/code"); }
		return;
	}
	/* check ICMP identifier */
	uint16_t const icmp_id  = icmp.query_id();
	if (icmp_id != ICMP_ID) {
		if (_verbose) {
			log("bad ICMP identifier"); }
		return;
	}
	/* check ICMP sequence number */
	uint16_t const icmp_seq  = icmp.query_seq();
	if (icmp_seq != _icmp_seq) {
		if (_verbose) {
			log("bad ICMP sequence number"); }
		return;
	}
	/* check ICMP data size */
	if (icmp_data_sz != _icmp_data_sz) {
		if (_verbose) {
			log("bad ICMP data size"); }
		return;
	}
	/* check ICMP data */
	struct Data { char chr[0]; };
	Data &data = icmp.data<Data>(_icmp_data_sz);
	char chr = 'a';
	for (addr_t chr_id = 0; chr_id < icmp_data_sz; chr_id++) {
		if (data.chr[chr_id] != chr) {
			if (_verbose) {
				log("bad ICMP data"); }
			return;
		}
		chr = chr < 'z' ? chr + 1 : 'a';
	}
	/* calculate time since the request was sent */
	unsigned long time_us = _timer.curr_time().trunc_to_plain_us().value - _send_time.value;
	unsigned long const time_ms = time_us / 1000UL;
	time_us = time_us - time_ms * 1000UL;

	/* print success message */
	log(icmp_data_sz + sizeof(Icmp_packet), " bytes from ", ip.src(),
	    ": icmp_seq=", icmp_seq, " ttl=", (unsigned long)IPV4_TIME_TO_LIVE,
	    " time=", time_ms, ".", time_us ," ms");

	/* raise ICMP sequence number and check exit condition */
	_icmp_seq++;
	_count--;
	if (!_count) {
		_env.parent().exit(0); }
}


void Main::_handle_icmp_dst_unreachbl(Ipv4_packet &ip,
                                      Icmp_packet &icmp,
                                      size_t       icmp_data_sz)
{
	/* drop packet if embedded IP checksum is invalid */
	Ipv4_packet &embed_ip = icmp.data<Ipv4_packet>(icmp_data_sz);
	if (Ipv4_packet::calculate_checksum(embed_ip) != embed_ip.checksum()) {
		if (_verbose) {
			log("bad IP checksum in payload of ICMP error"); }
		return;
	}
	/* drop packet if the ICMP error is not about ICMP */
	if (embed_ip.protocol() != Ipv4_packet::Protocol::ICMP) {
		if (_verbose) {
			log("bad IP protocol in payload of ICMP error"); }
		return;
	}
	/* drop packet if embedded ICMP identifier is invalid */
	size_t const embed_icmp_sz = embed_ip.total_length() - sizeof(Ipv4_packet);
	Icmp_packet &embed_icmp    = *embed_ip.data<Icmp_packet>(embed_icmp_sz);
	if (embed_icmp.query_id() != ICMP_ID) {
		if (_verbose) {
			log("bad ICMP identifier in payload of ICMP error"); }
		return;
	}
	/* drop packet if embedded ICMP sequence number is invalid */
	uint16_t const embed_icmp_seq = embed_icmp.query_seq();
	if (embed_icmp_seq != _icmp_seq) {
		if (_verbose) {
			log("bad ICMP sequence number in payload of ICMP error"); }
		return;
	}
	log("From ", ip.src(), " icmp_seq=", embed_icmp_seq, " Destination Unreachable");
}


void Main::_handle_icmp(Ipv4_packet  &ip,
                        size_t const  ip_size)
{
	/* drop packet if ICMP checksum is invalid */
	size_t const icmp_sz      = ip_size - sizeof(Ipv4_packet);
	Icmp_packet &icmp         = *ip.data<Icmp_packet>(icmp_sz);
	size_t const icmp_data_sz = icmp_sz - sizeof(Icmp_packet);
	if (icmp.calc_checksum(icmp_data_sz) != icmp.checksum()) {
		if (_verbose) {
			log("bad ICMP checksum"); }
		return;
	}
	/* select ICMP type */
	switch (icmp.type()) {
	case Icmp_packet::Type::ECHO_REPLY:      _handle_icmp_echo_reply(ip, icmp, icmp_data_sz); break;
	case Icmp_packet::Type::DST_UNREACHABLE: _handle_icmp_dst_unreachbl(ip, icmp, icmp_data_sz); break;
	default:
		if (_verbose) {
			log("bad ICMP type"); }
		return;
	}
}


void Main::_handle_arp(Ethernet_frame &eth,
                       size_t const    eth_size)
{
	/* check ARP protocol- and hardware address type */
	Arp_packet &arp = *eth.data<Arp_packet>(eth_size - sizeof(Ethernet_frame));
	if (!arp.ethernet_ipv4()) {
		error("ARP for unknown protocol"); }

	/* check ARP operation */
	switch (arp.opcode()) {
	case Arp_packet::REPLY:

		/* check whether we waited for this ARP reply */
		if (_dst_mac != Mac_address() || arp.src_ip() != _dst_ip) {
			return; }

		/* set destination MAC address and retry to ping */
		_dst_mac = arp.src_mac();
		_send_ping();
		return;

	case Arp_packet::REQUEST:

		/* check whether the ARP request targets us */
		if (arp.dst_ip() != _src_ip) {
			return; }

		_send_arp_reply(eth, arp);

	default: ; }
}


void Main::_ready_to_submit()
{
	while (_sink().packet_avail()) {

		Packet_descriptor const pkt = _sink().get_packet();
		if (!pkt.size()) {
			continue; }

		_handle_eth(_sink().packet_content(pkt), pkt.size(), pkt);

		if (!_sink().ready_to_ack()) {
			error("ack state FULL");
			return;
		}
		_sink().acknowledge_packet(pkt);
	}
}


void Main::_ready_to_ack()
{
	while (_source().ack_avail()) {
		_source().release_packet(_source().get_acked_packet()); }
}


void Main::_send_arp_reply(Ethernet_frame &req_eth,
                           Arp_packet     &req_arp)
{
	enum {
		ETH_HDR_SZ = sizeof(Ethernet_frame),
		ETH_DAT_SZ = sizeof(Arp_packet) + ETH_HDR_SZ >= Ethernet_frame::MIN_SIZE ?
		             sizeof(Arp_packet) :
		             Ethernet_frame::MIN_SIZE - ETH_HDR_SZ,
		ETH_CRC_SZ = sizeof(uint32_t),
		PKT_SIZE   = ETH_HDR_SZ + ETH_DAT_SZ + ETH_CRC_SZ,
	};
	_send(PKT_SIZE, [&] (void *pkt_base) {

		/* write Ethernet header */
		Ethernet_frame &eth = *reinterpret_cast<Ethernet_frame *>(pkt_base);
		eth.dst(req_eth.src());
		eth.src(_src_mac);
		eth.type(Ethernet_frame::Type::ARP);

		/* write ARP header */
		Arp_packet &arp = *eth.data<Arp_packet>(PKT_SIZE - sizeof(Ethernet_frame));
		arp.hardware_address_type(Arp_packet::ETHERNET);
		arp.protocol_address_type(Arp_packet::IPV4);
		arp.hardware_address_size(sizeof(Mac_address));
		arp.protocol_address_size(sizeof(Ipv4_address));
		arp.opcode(Arp_packet::REPLY);
		arp.src_mac(_src_mac);
		arp.src_ip(_src_ip);
		arp.dst_mac(req_eth.src());
		arp.dst_ip(req_arp.src_ip());
	});
}


void Main::_broadcast_arp_request()
{
	enum {
		ETH_HDR_SZ = sizeof(Ethernet_frame),
		ETH_DAT_SZ = sizeof(Arp_packet) + ETH_HDR_SZ >= Ethernet_frame::MIN_SIZE ?
		             sizeof(Arp_packet) :
		             Ethernet_frame::MIN_SIZE - ETH_HDR_SZ,
		ETH_CRC_SZ = sizeof(uint32_t),
		PKT_SIZE   = ETH_HDR_SZ + ETH_DAT_SZ + ETH_CRC_SZ,
	};
	_send(PKT_SIZE, [&] (void *pkt_base) {

		/* write Ethernet header */
		Ethernet_frame &eth = *reinterpret_cast<Ethernet_frame *>(pkt_base);
		eth.dst(Mac_address(0xff));
		eth.src(_src_mac);
		eth.type(Ethernet_frame::Type::ARP);

		/* write ARP header */
		Arp_packet &arp = *eth.data<Arp_packet>(PKT_SIZE - sizeof(Ethernet_frame));
		arp.hardware_address_type(Arp_packet::ETHERNET);
		arp.protocol_address_type(Arp_packet::IPV4);
		arp.hardware_address_size(sizeof(Mac_address));
		arp.protocol_address_size(sizeof(Ipv4_address));
		arp.opcode(Arp_packet::REQUEST);
		arp.src_mac(_src_mac);
		arp.src_ip(_src_ip);
		arp.dst_mac(Mac_address(0xff));
		arp.dst_ip(_dst_ip);
	});
}


void Main::_send_ping(Duration)
{
	if (_dst_mac == Mac_address()) {
		_broadcast_arp_request();
		return;
	}
	size_t const buf_sz = sizeof(Ethernet_frame) + sizeof(Ipv4_packet) +
	                      sizeof(Icmp_packet) + _icmp_data_sz;

	_send(buf_sz, [&] (void *pkt_base) {

		/* create ETH header */
		Size_guard<Send_buffer_too_small> size(buf_sz);
		size.add(sizeof(Ethernet_frame));
		Ethernet_frame &eth = *reinterpret_cast<Ethernet_frame *>(pkt_base);
		eth.dst(_dst_mac);
		eth.src(_src_mac);
		eth.type(Ethernet_frame::Type::IPV4);

		/* create IP header */
		size_t const ip_off = size.curr();
		Ipv4_packet &ip = *eth.data<Ipv4_packet>(size.left());
		size.add(sizeof(Ipv4_packet));
		ip.header_length(sizeof(Ipv4_packet) / 4);
		ip.version(4);
		ip.diff_service(0);
		ip.ecn(0);
		ip.identification(0);
		ip.flags(0);
		ip.fragment_offset(0);
		ip.time_to_live(IPV4_TIME_TO_LIVE);
		ip.protocol(Ipv4_packet::Protocol::ICMP);
		ip.src(_src_ip);
		ip.dst(_dst_ip);

		/* create ICMP header */
		Icmp_packet &icmp = *ip.data<Icmp_packet>(size.left());
		size.add(sizeof(Icmp_packet) + _icmp_data_sz);
		icmp.type(Icmp_packet::Type::ECHO_REQUEST);
		icmp.code(Icmp_packet::Code::ECHO_REQUEST);
		icmp.query_id(ICMP_ID);
		icmp.query_seq(_icmp_seq);

		/* fill ICMP data with characters from 'a' to 'w' */
		struct Data { char chr[0]; };
		Data &data = icmp.data<Data>(_icmp_data_sz);
		char chr = 'a';
		for (addr_t chr_id = 0; chr_id < _icmp_data_sz; chr_id++) {
			data.chr[chr_id] = chr;
			chr = chr < 'z' ? chr + 1 : 'a';
		}
		/* fill in header values that require the packet to be complete */
		icmp.checksum(icmp.calc_checksum(_icmp_data_sz));
		ip.total_length(size.curr() - ip_off);
		ip.checksum(Ipv4_packet::calculate_checksum(ip));
	});
	_send_time = _timer.curr_time().trunc_to_plain_us();
}


void Component::construct(Env &env) { static Main main(env); }
