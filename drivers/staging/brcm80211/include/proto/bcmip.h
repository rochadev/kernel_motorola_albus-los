/*
 * Copyright (c) 2010 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _bcmip_h_
#define _bcmip_h_

#ifndef _TYPEDEFS_H_
#include <typedefs.h>
#endif

#include <packed_section_start.h>

#define IP_VER_OFFSET		0x0
#define IP_VER_MASK		0xf0
#define IP_VER_SHIFT		4
#define IP_VER_4		4
#define IP_VER_6		6

#define IP_VER(ip_body) \
	((((uint8 *)(ip_body))[IP_VER_OFFSET] & IP_VER_MASK) >> IP_VER_SHIFT)

#define IP_PROT_ICMP		0x1
#define IP_PROT_TCP		0x6
#define IP_PROT_UDP		0x11

#define IPV4_VER_HL_OFFSET	0
#define IPV4_TOS_OFFSET		1
#define IPV4_PKTLEN_OFFSET	2
#define IPV4_PKTFLAG_OFFSET	6
#define IPV4_PROT_OFFSET	9
#define IPV4_CHKSUM_OFFSET	10
#define IPV4_SRC_IP_OFFSET	12
#define IPV4_DEST_IP_OFFSET	16
#define IPV4_OPTIONS_OFFSET	20

#define IPV4_VER_MASK		0xf0
#define IPV4_VER_SHIFT		4

#define IPV4_HLEN_MASK		0x0f
#define IPV4_HLEN(ipv4_body)	\
	(4 * (((uint8 *)(ipv4_body))[IPV4_VER_HL_OFFSET] & IPV4_HLEN_MASK))

#define IPV4_ADDR_LEN		4

#define IPV4_ADDR_NULL(a)	((((uint8 *)(a))[0] | ((uint8 *)(a))[1] | \
	((uint8 *)(a))[2] | ((uint8 *)(a))[3]) == 0)

#define IPV4_ADDR_BCAST(a)	((((uint8 *)(a))[0] & ((uint8 *)(a))[1] & \
	((uint8 *)(a))[2] & ((uint8 *)(a))[3]) == 0xff)

#define	IPV4_TOS_DSCP_MASK	0xfc
#define	IPV4_TOS_DSCP_SHIFT	2

#define	IPV4_TOS(ipv4_body)	(((uint8 *)(ipv4_body))[IPV4_TOS_OFFSET])

#define	IPV4_TOS_PREC_MASK	0xe0
#define	IPV4_TOS_PREC_SHIFT	5

#define IPV4_TOS_LOWDELAY	0x10
#define IPV4_TOS_THROUGHPUT	0x8
#define IPV4_TOS_RELIABILITY	0x4

#define IPV4_PROT(ipv4_body)	(((uint8 *)(ipv4_body))[IPV4_PROT_OFFSET])

#define IPV4_FRAG_RESV		0x8000
#define IPV4_FRAG_DONT		0x4000
#define IPV4_FRAG_MORE		0x2000
#define IPV4_FRAG_OFFSET_MASK	0x1fff

#define IPV4_ADDR_STR_LEN	16

BWL_PRE_PACKED_STRUCT struct ipv4_addr {
	uint8 addr[IPV4_ADDR_LEN];
} BWL_POST_PACKED_STRUCT;

BWL_PRE_PACKED_STRUCT struct ipv4_hdr {
	uint8 version_ihl;
	uint8 tos;
	uint16 tot_len;
	uint16 id;
	uint16 frag;
	uint8 ttl;
	uint8 prot;
	uint16 hdr_chksum;
	uint8 src_ip[IPV4_ADDR_LEN];
	uint8 dst_ip[IPV4_ADDR_LEN];
} BWL_POST_PACKED_STRUCT;

#define IPV6_PAYLOAD_LEN_OFFSET	4
#define IPV6_NEXT_HDR_OFFSET	6
#define IPV6_HOP_LIMIT_OFFSET	7
#define IPV6_SRC_IP_OFFSET	8
#define IPV6_DEST_IP_OFFSET	24

#define IPV6_TRAFFIC_CLASS(ipv6_body) \
	(((((uint8 *)(ipv6_body))[0] & 0x0f) << 4) | \
	 ((((uint8 *)(ipv6_body))[1] & 0xf0) >> 4))

#define IPV6_FLOW_LABEL(ipv6_body) \
	(((((uint8 *)(ipv6_body))[1] & 0x0f) << 16) | \
	 (((uint8 *)(ipv6_body))[2] << 8) | \
	 (((uint8 *)(ipv6_body))[3]))

#define IPV6_PAYLOAD_LEN(ipv6_body) \
	((((uint8 *)(ipv6_body))[IPV6_PAYLOAD_LEN_OFFSET + 0] << 8) | \
	 ((uint8 *)(ipv6_body))[IPV6_PAYLOAD_LEN_OFFSET + 1])

#define IPV6_NEXT_HDR(ipv6_body) \
	(((uint8 *)(ipv6_body))[IPV6_NEXT_HDR_OFFSET])

#define IPV6_PROT(ipv6_body)	IPV6_NEXT_HDR(ipv6_body)

#define IPV6_ADDR_LEN		16

#ifndef IP_TOS
#define IP_TOS(ip_body) \
	(IP_VER(ip_body) == IP_VER_4 ? IPV4_TOS(ip_body) : \
	 IP_VER(ip_body) == IP_VER_6 ? IPV6_TRAFFIC_CLASS(ip_body) : 0)
#endif

#include <packed_section_end.h>

#endif				/* _bcmip_h_ */
