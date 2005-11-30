/* (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2004 Netfilter Core Team <coreteam@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>:
 *	- Real stateful connection tracking
 *	- Modified state transitions table
 *	- Window scaling support added
 *	- SACK support added
 *
 * Willy Tarreau:
 *	- State table bugfixes
 *	- More robust state changes
 *	- Tuning timer parameters
 *
 * version 2.2
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/netfilter.h>
#include <linux/module.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/spinlock.h>

#include <net/tcp.h>

#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv4/ip_conntrack.h>
#include <linux/netfilter_ipv4/ip_conntrack_protocol.h>

#if 0
#define DEBUGP printk
#define DEBUGP_VARS
#else
#define DEBUGP(format, args...)
#endif

/* Protects conntrack->proto.tcp */
static DEFINE_RWLOCK(tcp_lock);

/* "Be conservative in what you do, 
    be liberal in what you accept from others." 
    If it's non-zero, we mark only out of window RST segments as INVALID. */
int ip_ct_tcp_be_liberal = 0;

/* When connection is picked up from the middle, how many packets are required
   to pass in each direction when we assume we are in sync - if any side uses
   window scaling, we lost the game. 
   If it is set to zero, we disable picking up already established 
   connections. */
int ip_ct_tcp_loose = 3;

/* Max number of the retransmitted packets without receiving an (acceptable) 
   ACK from the destination. If this number is reached, a shorter timer 
   will be started. */
int ip_ct_tcp_max_retrans = 3;

  /* FIXME: Examine ipfilter's timeouts and conntrack transitions more
     closely.  They're more complex. --RR */

static const char *tcp_conntrack_names[] = {
	"NONE",
	"SYN_SENT",
	"SYN_RECV",
	"ESTABLISHED",
	"FIN_WAIT",
	"CLOSE_WAIT",
	"LAST_ACK",
	"TIME_WAIT",
	"CLOSE",
	"LISTEN"
};
  
#define SECS * HZ
#define MINS * 60 SECS
#define HOURS * 60 MINS
#define DAYS * 24 HOURS

unsigned long ip_ct_tcp_timeout_syn_sent =      2 MINS;
unsigned long ip_ct_tcp_timeout_syn_recv =     60 SECS;
unsigned long ip_ct_tcp_timeout_established =   5 DAYS;
unsigned long ip_ct_tcp_timeout_fin_wait =      2 MINS;
unsigned long ip_ct_tcp_timeout_close_wait =   60 SECS;
unsigned long ip_ct_tcp_timeout_last_ack =     30 SECS;
unsigned long ip_ct_tcp_timeout_time_wait =     2 MINS;
unsigned long ip_ct_tcp_timeout_close =        10 SECS;

/* RFC1122 says the R2 limit should be at least 100 seconds.
   Linux uses 15 packets as limit, which corresponds 
   to ~13-30min depending on RTO. */
unsigned long ip_ct_tcp_timeout_max_retrans =     5 MINS;
 
static const unsigned long * tcp_timeouts[]
= { NULL,                              /*      TCP_CONNTRACK_NONE */
    &ip_ct_tcp_timeout_syn_sent,       /*      TCP_CONNTRACK_SYN_SENT, */
    &ip_ct_tcp_timeout_syn_recv,       /*      TCP_CONNTRACK_SYN_RECV, */
    &ip_ct_tcp_timeout_established,    /*      TCP_CONNTRACK_ESTABLISHED,      */
    &ip_ct_tcp_timeout_fin_wait,       /*      TCP_CONNTRACK_FIN_WAIT, */
    &ip_ct_tcp_timeout_close_wait,     /*      TCP_CONNTRACK_CLOSE_WAIT,       */
    &ip_ct_tcp_timeout_last_ack,       /*      TCP_CONNTRACK_LAST_ACK, */
    &ip_ct_tcp_timeout_time_wait,      /*      TCP_CONNTRACK_TIME_WAIT,        */
    &ip_ct_tcp_timeout_close,          /*      TCP_CONNTRACK_CLOSE,    */
    NULL,                              /*      TCP_CONNTRACK_LISTEN */
 };
 
#define sNO TCP_CONNTRACK_NONE
#define sSS TCP_CONNTRACK_SYN_SENT
#define sSR TCP_CONNTRACK_SYN_RECV
#define sES TCP_CONNTRACK_ESTABLISHED
#define sFW TCP_CONNTRACK_FIN_WAIT
#define sCW TCP_CONNTRACK_CLOSE_WAIT
#define sLA TCP_CONNTRACK_LAST_ACK
#define sTW TCP_CONNTRACK_TIME_WAIT
#define sCL TCP_CONNTRACK_CLOSE
#define sLI TCP_CONNTRACK_LISTEN
#define sIV TCP_CONNTRACK_MAX
#define sIG TCP_CONNTRACK_IGNORE

/* What TCP flags are set from RST/SYN/FIN/ACK. */
enum tcp_bit_set {
	TCP_SYN_SET,
	TCP_SYNACK_SET,
	TCP_FIN_SET,
	TCP_ACK_SET,
	TCP_RST_SET,
	TCP_NONE_SET,
};
  
/*
 * The TCP state transition table needs a few words...
 *
 * We are the man in the middle. All the packets go through us
 * but might get lost in transit to the destination.
 * It is assumed that the destinations can't receive segments 
 * we haven't seen.
 *
 * The checked segment is in window, but our windows are *not*
 * equivalent with the ones of the sender/receiver. We always
 * try to guess the state of the current sender.
 *
 * The meaning of the states are:
 *
 * NONE:	initial state
 * SYN_SENT:	SYN-only packet seen 
 * SYN_RECV:	SYN-ACK packet seen
 * ESTABLISHED:	ACK packet seen
 * FIN_WAIT:	FIN packet seen
 * CLOSE_WAIT:	ACK seen (after FIN) 
 * LAST_ACK:	FIN seen (after FIN)
 * TIME_WAIT:	last ACK seen
 * CLOSE:	closed connection
 *
 * LISTEN state is not used.
 *
 * Packets marked as IGNORED (sIG):
 *	if they may be either invalid or valid 
 *	and the receiver may send back a connection 
 *	closing RST or a SYN/ACK.
 *
 * Packets marked as INVALID (sIV):
 *	if they are invalid
 *	or we do not support the request (simultaneous open)
 */
static const enum tcp_conntrack tcp_conntracks[2][6][TCP_CONNTRACK_MAX] = {
	{
/* ORIGINAL */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*syn*/	   { sSS, sSS, sIG, sIG, sIG, sIG, sIG, sSS, sSS, sIV },
/*
 *	sNO -> sSS	Initialize a new connection
 *	sSS -> sSS	Retransmitted SYN
 *	sSR -> sIG	Late retransmitted SYN?
 *	sES -> sIG	Error: SYNs in window outside the SYN_SENT state
 *			are errors. Receiver will reply with RST 
 *			and close the connection.
 *			Or we are not in sync and hold a dead connection.
 *	sFW -> sIG
 *	sCW -> sIG
 *	sLA -> sIG
 *	sTW -> sSS	Reopened connection (RFC 1122).
 *	sCL -> sSS
 */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*synack*/ { sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV },
/*
 * A SYN/ACK from the client is always invalid:
 *	- either it tries to set up a simultaneous open, which is 
 *	  not supported;
 *	- or the firewall has just been inserted between the two hosts
 *	  during the session set-up. The SYN will be retransmitted 
 *	  by the true client (or it'll time out).
 */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*fin*/    { sIV, sIV, sFW, sFW, sLA, sLA, sLA, sTW, sCL, sIV },
/*
 *	sNO -> sIV	Too late and no reason to do anything...
 *	sSS -> sIV	Client migth not send FIN in this state:
 *			we enforce waiting for a SYN/ACK reply first.
 *	sSR -> sFW	Close started.
 *	sES -> sFW	
 *	sFW -> sLA	FIN seen in both directions, waiting for
 *			the last ACK. 
 *			Migth be a retransmitted FIN as well...
 *	sCW -> sLA
 *	sLA -> sLA	Retransmitted FIN. Remain in the same state.
 *	sTW -> sTW
 *	sCL -> sCL
 */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*ack*/	   { sES, sIV, sES, sES, sCW, sCW, sTW, sTW, sCL, sIV },
/*
 *	sNO -> sES	Assumed.
 *	sSS -> sIV	ACK is invalid: we haven't seen a SYN/ACK yet.
 *	sSR -> sES	Established state is reached.
 *	sES -> sES	:-)
 *	sFW -> sCW	Normal close request answered by ACK.
 *	sCW -> sCW
 *	sLA -> sTW	Last ACK detected.
 *	sTW -> sTW	Retransmitted last ACK. Remain in the same state.
 *	sCL -> sCL
 */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*rst*/    { sIV, sCL, sCL, sCL, sCL, sCL, sCL, sCL, sCL, sIV },
/*none*/   { sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV }
	},
	{
/* REPLY */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*syn*/	   { sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV },
/*
 *	sNO -> sIV	Never reached.
 *	sSS -> sIV	Simultaneous open, not supported
 *	sSR -> sIV	Simultaneous open, not supported.
 *	sES -> sIV	Server may not initiate a connection.
 *	sFW -> sIV
 *	sCW -> sIV
 *	sLA -> sIV
 *	sTW -> sIV	Reopened connection, but server may not do it.
 *	sCL -> sIV
 */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*synack*/ { sIV, sSR, sSR, sIG, sIG, sIG, sIG, sIG, sIG, sIV },
/*
 *	sSS -> sSR	Standard open.
 *	sSR -> sSR	Retransmitted SYN/ACK.
 *	sES -> sIG	Late retransmitted SYN/ACK?
 *	sFW -> sIG	Might be SYN/ACK answering ignored SYN
 *	sCW -> sIG
 *	sLA -> sIG
 *	sTW -> sIG
 *	sCL -> sIG
 */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*fin*/    { sIV, sIV, sFW, sFW, sLA, sLA, sLA, sTW, sCL, sIV },
/*
 *	sSS -> sIV	Server might not send FIN in this state.
 *	sSR -> sFW	Close started.
 *	sES -> sFW
 *	sFW -> sLA	FIN seen in both directions.
 *	sCW -> sLA
 *	sLA -> sLA	Retransmitted FIN.
 *	sTW -> sTW
 *	sCL -> sCL
 */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*ack*/	   { sIV, sIV, sSR, sES, sCW, sCW, sTW, sTW, sCL, sIV },
/*
 *	sSS -> sIV	Might be a half-open connection.
 *	sSR -> sSR	Might answer late resent SYN.
 *	sES -> sES	:-)
 *	sFW -> sCW	Normal close request answered by ACK.
 *	sCW -> sCW
 *	sLA -> sTW	Last ACK detected.
 *	sTW -> sTW	Retransmitted last ACK.
 *	sCL -> sCL
 */
/* 	     sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sLI	*/
/*rst*/    { sIV, sCL, sCL, sCL, sCL, sCL, sCL, sCL, sCL, sIV },
/*none*/   { sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV }
  	}
};

static int tcp_pkt_to_tuple(const struct sk_buff *skb,
			    unsigned int dataoff,
			    struct ip_conntrack_tuple *tuple)
{
	struct tcphdr _hdr, *hp;

	/* Actually only need first 8 bytes. */
	hp = skb_header_pointer(skb, dataoff, 8, &_hdr);
	if (hp == NULL)
		return 0;

	tuple->src.u.tcp.port = hp->source;
	tuple->dst.u.tcp.port = hp->dest;

	return 1;
}

static int tcp_invert_tuple(struct ip_conntrack_tuple *tuple,
			    const struct ip_conntrack_tuple *orig)
{
	tuple->src.u.tcp.port = orig->dst.u.tcp.port;
	tuple->dst.u.tcp.port = orig->src.u.tcp.port;
	return 1;
}

/* Print out the per-protocol part of the tuple. */
static int tcp_print_tuple(struct seq_file *s,
			   const struct ip_conntrack_tuple *tuple)
{
	return seq_printf(s, "sport=%hu dport=%hu ",
			  ntohs(tuple->src.u.tcp.port),
			  ntohs(tuple->dst.u.tcp.port));
}

/* Print out the private part of the conntrack. */
static int tcp_print_conntrack(struct seq_file *s,
			       const struct ip_conntrack *conntrack)
{
	enum tcp_conntrack state;

	read_lock_bh(&tcp_lock);
	state = conntrack->proto.tcp.state;
	read_unlock_bh(&tcp_lock);

	return seq_printf(s, "%s ", tcp_conntrack_names[state]);
}

#if defined(CONFIG_IP_NF_CONNTRACK_NETLINK) || \
    defined(CONFIG_IP_NF_CONNTRACK_NETLINK_MODULE)
static int tcp_to_nfattr(struct sk_buff *skb, struct nfattr *nfa,
			 const struct ip_conntrack *ct)
{
	struct nfattr *nest_parms = NFA_NEST(skb, CTA_PROTOINFO_TCP);
	
	read_lock_bh(&tcp_lock);
	NFA_PUT(skb, CTA_PROTOINFO_TCP_STATE, sizeof(u_int8_t),
		&ct->proto.tcp.state);
	read_unlock_bh(&tcp_lock);

	NFA_NEST_END(skb, nest_parms);

	return 0;

nfattr_failure:
	read_unlock_bh(&tcp_lock);
	return -1;
}

static const size_t cta_min_tcp[CTA_PROTOINFO_TCP_MAX] = {
	[CTA_PROTOINFO_TCP_STATE-1]	= sizeof(u_int8_t),
};

static int nfattr_to_tcp(struct nfattr *cda[], struct ip_conntrack *ct)
{
	struct nfattr *attr = cda[CTA_PROTOINFO_TCP-1];
	struct nfattr *tb[CTA_PROTOINFO_TCP_MAX];

	/* updates could not contain anything about the private
	 * protocol info, in that case skip the parsing */
	if (!attr)
		return 0;

        nfattr_parse_nested(tb, CTA_PROTOINFO_TCP_MAX, attr);

	if (nfattr_bad_size(tb, CTA_PROTOINFO_TCP_MAX, cta_min_tcp))
		return -EINVAL;

	if (!tb[CTA_PROTOINFO_TCP_STATE-1])
		return -EINVAL;

	write_lock_bh(&tcp_lock);
	ct->proto.tcp.state = 
		*(u_int8_t *)NFA_DATA(tb[CTA_PROTOINFO_TCP_STATE-1]);
	write_unlock_bh(&tcp_lock);

	return 0;
}
#endif

static unsigned int get_conntrack_index(const struct tcphdr *tcph)
{
	if (tcph->rst) return TCP_RST_SET;
	else if (tcph->syn) return (tcph->ack ? TCP_SYNACK_SET : TCP_SYN_SET);
	else if (tcph->fin) return TCP_FIN_SET;
	else if (tcph->ack) return TCP_ACK_SET;
	else return TCP_NONE_SET;
}

/* TCP connection tracking based on 'Real Stateful TCP Packet Filtering
   in IP Filter' by Guido van Rooij.
   
   http://www.nluug.nl/events/sane2000/papers.html
   http://www.iae.nl/users/guido/papers/tcp_filtering.ps.gz
   
   The boundaries and the conditions are changed according to RFC793:
   the packet must intersect the window (i.e. segments may be
   after the right or before the left edge) and thus receivers may ACK
   segments after the right edge of the window.

   	td_maxend = max(sack + max(win,1)) seen in reply packets
	td_maxwin = max(max(win, 1)) + (sack - ack) seen in sent packets
	td_maxwin += seq + len - sender.td_maxend
			if seq + len > sender.td_maxend
	td_end    = max(seq + len) seen in sent packets
   
   I.   Upper bound for valid data:	seq <= sender.td_maxend
   II.  Lower bound for valid data:	seq + len >= sender.td_end - receiver.td_maxwin
   III.	Upper bound for valid ack:      sack <= receiver.td_end
   IV.	Lower bound for valid ack:	ack >= receiver.td_end - MAXACKWINDOW
   	
   where sack is the highest right edge of sack block found in the packet.
   	
   The upper bound limit for a valid ack is not ignored - 
   we doesn't have to deal with fragments. 
*/

static inline __u32 segment_seq_plus_len(__u32 seq,
					 size_t len,
					 struct iphdr *iph,
					 struct tcphdr *tcph)
{
	return (seq + len - (iph->ihl + tcph->doff)*4
		+ (tcph->syn ? 1 : 0) + (tcph->fin ? 1 : 0));
}
  
/* Fixme: what about big packets? */
#define MAXACKWINCONST			66000
#define MAXACKWINDOW(sender)						\
	((sender)->td_maxwin > MAXACKWINCONST ? (sender)->td_maxwin	\
					      : MAXACKWINCONST)
  
/*
 * Simplified tcp_parse_options routine from tcp_input.c
 */
static void tcp_options(const struct sk_buff *skb,
			struct iphdr *iph,
			struct tcphdr *tcph, 
			struct ip_ct_tcp_state *state)
{
	unsigned char buff[(15 * 4) - sizeof(struct tcphdr)];
	unsigned char *ptr;
	int length = (tcph->doff*4) - sizeof(struct tcphdr);
	
	if (!length)
		return;

	ptr = skb_header_pointer(skb,
				 (iph->ihl * 4) + sizeof(struct tcphdr),
				 length, buff);
	BUG_ON(ptr == NULL);

	state->td_scale = 
	state->flags = 0;
	
	while (length > 0) {
		int opcode=*ptr++;
		int opsize;
		
		switch (opcode) {
		case TCPOPT_EOL:
			return;
		case TCPOPT_NOP:	/* Ref: RFC 793 section 3.1 */
			length--;
			continue;
		default:
			opsize=*ptr++;
			if (opsize < 2) /* "silly options" */
				return;
			if (opsize > length)
				break;	/* don't parse partial options */

			if (opcode == TCPOPT_SACK_PERM 
			    && opsize == TCPOLEN_SACK_PERM)
				state->flags |= IP_CT_TCP_FLAG_SACK_PERM;
			else if (opcode == TCPOPT_WINDOW
				 && opsize == TCPOLEN_WINDOW) {
				state->td_scale = *(u_int8_t *)ptr;
				
				if (state->td_scale > 14) {
					/* See RFC1323 */
					state->td_scale = 14;
				}
				state->flags |=
					IP_CT_TCP_FLAG_WINDOW_SCALE;
			}
			ptr += opsize - 2;
			length -= opsize;
		}
	}
}

static void tcp_sack(const struct sk_buff *skb,
		     struct iphdr *iph,
		     struct tcphdr *tcph,
		     __u32 *sack)
{
	unsigned char buff[(15 * 4) - sizeof(struct tcphdr)];
	unsigned char *ptr;
	int length = (tcph->doff*4) - sizeof(struct tcphdr);
	__u32 tmp;

	if (!length)
		return;

	ptr = skb_header_pointer(skb,
				 (iph->ihl * 4) + sizeof(struct tcphdr),
				 length, buff);
	BUG_ON(ptr == NULL);

	/* Fast path for timestamp-only option */
	if (length == TCPOLEN_TSTAMP_ALIGNED*4
	    && *(__u32 *)ptr ==
	        __constant_ntohl((TCPOPT_NOP << 24) 
	        		 | (TCPOPT_NOP << 16)
	        		 | (TCPOPT_TIMESTAMP << 8)
	        		 | TCPOLEN_TIMESTAMP))
		return;
		
	while (length > 0) {
		int opcode=*ptr++;
		int opsize, i;
		
		switch (opcode) {
		case TCPOPT_EOL:
			return;
		case TCPOPT_NOP:	/* Ref: RFC 793 section 3.1 */
			length--;
			continue;
		default:
			opsize=*ptr++;
			if (opsize < 2) /* "silly options" */
				return;
			if (opsize > length)
				break;	/* don't parse partial options */

			if (opcode == TCPOPT_SACK 
			    && opsize >= (TCPOLEN_SACK_BASE 
			    		  + TCPOLEN_SACK_PERBLOCK)
			    && !((opsize - TCPOLEN_SACK_BASE) 
			    	 % TCPOLEN_SACK_PERBLOCK)) {
			    	for (i = 0;
			    	     i < (opsize - TCPOLEN_SACK_BASE);
			    	     i += TCPOLEN_SACK_PERBLOCK) {
					tmp = ntohl(*((u_int32_t *)(ptr+i)+1));
					
					if (after(tmp, *sack))
						*sack = tmp;
				}
				return;
			}
			ptr += opsize - 2;
			length -= opsize;
		}
	}
}

static int tcp_in_window(struct ip_ct_tcp *state, 
                         enum ip_conntrack_dir dir,
                         unsigned int index,
                         const struct sk_buff *skb,
                         struct iphdr *iph,
                         struct tcphdr *tcph)
{
	struct ip_ct_tcp_state *sender = &state->seen[dir];
	struct ip_ct_tcp_state *receiver = &state->seen[!dir];
	__u32 seq, ack, sack, end, win, swin;
	int res;
	
	/*
	 * Get the required data from the packet.
	 */
	seq = ntohl(tcph->seq);
	ack = sack = ntohl(tcph->ack_seq);
	win = ntohs(tcph->window);
	end = segment_seq_plus_len(seq, skb->len, iph, tcph);
	
	if (receiver->flags & IP_CT_TCP_FLAG_SACK_PERM)
		tcp_sack(skb, iph, tcph, &sack);
		
	DEBUGP("tcp_in_window: START\n");
	DEBUGP("tcp_in_window: src=%u.%u.%u.%u:%hu dst=%u.%u.%u.%u:%hu "
	       "seq=%u ack=%u sack=%u win=%u end=%u\n",
		NIPQUAD(iph->saddr), ntohs(tcph->source), 
		NIPQUAD(iph->daddr), ntohs(tcph->dest),
		seq, ack, sack, win, end);
	DEBUGP("tcp_in_window: sender end=%u maxend=%u maxwin=%u scale=%i "
	       "receiver end=%u maxend=%u maxwin=%u scale=%i\n",
		sender->td_end, sender->td_maxend, sender->td_maxwin,
		sender->td_scale, 
		receiver->td_end, receiver->td_maxend, receiver->td_maxwin, 
		receiver->td_scale);
		
	if (sender->td_end == 0) {
		/*
		 * Initialize sender data.
		 */
		if (tcph->syn && tcph->ack) {
			/*
			 * Outgoing SYN-ACK in reply to a SYN.
			 */
			sender->td_end = 
			sender->td_maxend = end;
			sender->td_maxwin = (win == 0 ? 1 : win);

			tcp_options(skb, iph, tcph, sender);
			/* 
			 * RFC 1323:
			 * Both sides must send the Window Scale option
			 * to enable window scaling in either direction.
			 */
			if (!(sender->flags & IP_CT_TCP_FLAG_WINDOW_SCALE
			      && receiver->flags & IP_CT_TCP_FLAG_WINDOW_SCALE))
				sender->td_scale = 
				receiver->td_scale = 0;
		} else {
			/*
			 * We are in the middle of a connection,
			 * its history is lost for us.
			 * Let's try to use the data from the packet.
		 	 */
			sender->td_end = end;
			sender->td_maxwin = (win == 0 ? 1 : win);
			sender->td_maxend = end + sender->td_maxwin;
		}
	} else if (((state->state == TCP_CONNTRACK_SYN_SENT
		     && dir == IP_CT_DIR_ORIGINAL)
		    || (state->state == TCP_CONNTRACK_SYN_RECV
		        && dir == IP_CT_DIR_REPLY))
		    && after(end, sender->td_end)) {
		/*
		 * RFC 793: "if a TCP is reinitialized ... then it need
		 * not wait at all; it must only be sure to use sequence 
		 * numbers larger than those recently used."
		 */
		sender->td_end =
		sender->td_maxend = end;
		sender->td_maxwin = (win == 0 ? 1 : win);

		tcp_options(skb, iph, tcph, sender);
	}
	
	if (!(tcph->ack)) {
		/*
		 * If there is no ACK, just pretend it was set and OK.
		 */
		ack = sack = receiver->td_end;
	} else if (((tcp_flag_word(tcph) & (TCP_FLAG_ACK|TCP_FLAG_RST)) == 
		    (TCP_FLAG_ACK|TCP_FLAG_RST)) 
		   && (ack == 0)) {
		/*
		 * Broken TCP stacks, that set ACK in RST packets as well
		 * with zero ack value.
		 */
		ack = sack = receiver->td_end;
	}

	if (seq == end
	    && (!tcph->rst 
	        || (seq == 0 && state->state == TCP_CONNTRACK_SYN_SENT)))
		/*
		 * Packets contains no data: we assume it is valid
		 * and check the ack value only.
		 * However RST segments are always validated by their
		 * SEQ number, except when seq == 0 (reset sent answering
		 * SYN.
		 */
		seq = end = sender->td_end;
		
	DEBUGP("tcp_in_window: src=%u.%u.%u.%u:%hu dst=%u.%u.%u.%u:%hu "
	       "seq=%u ack=%u sack =%u win=%u end=%u\n",
		NIPQUAD(iph->saddr), ntohs(tcph->source),
		NIPQUAD(iph->daddr), ntohs(tcph->dest),
		seq, ack, sack, win, end);
	DEBUGP("tcp_in_window: sender end=%u maxend=%u maxwin=%u scale=%i "
	       "receiver end=%u maxend=%u maxwin=%u scale=%i\n",
		sender->td_end, sender->td_maxend, sender->td_maxwin,
		sender->td_scale, 
		receiver->td_end, receiver->td_maxend, receiver->td_maxwin,
		receiver->td_scale);
	
	DEBUGP("tcp_in_window: I=%i II=%i III=%i IV=%i\n",
		before(seq, sender->td_maxend + 1),
	    	after(end, sender->td_end - receiver->td_maxwin - 1),
	    	before(sack, receiver->td_end + 1),
	    	after(ack, receiver->td_end - MAXACKWINDOW(sender)));
	
	if (sender->loose || receiver->loose ||
	    (before(seq, sender->td_maxend + 1) &&
	     after(end, sender->td_end - receiver->td_maxwin - 1) &&
	     before(sack, receiver->td_end + 1) &&
	     after(ack, receiver->td_end - MAXACKWINDOW(sender)))) {
	    	/*
		 * Take into account window scaling (RFC 1323).
		 */
		if (!tcph->syn)
			win <<= sender->td_scale;
		
		/*
		 * Update sender data.
		 */
		swin = win + (sack - ack);
		if (sender->td_maxwin < swin)
			sender->td_maxwin = swin;
		if (after(end, sender->td_end))
			sender->td_end = end;
		/*
		 * Update receiver data.
		 */
		if (after(end, sender->td_maxend))
			receiver->td_maxwin += end - sender->td_maxend;
		if (after(sack + win, receiver->td_maxend - 1)) {
			receiver->td_maxend = sack + win;
			if (win == 0)
				receiver->td_maxend++;
		}

		/* 
		 * Check retransmissions.
		 */
		if (index == TCP_ACK_SET) {
			if (state->last_dir == dir
			    && state->last_seq == seq
			    && state->last_ack == ack
			    && state->last_end == end)
				state->retrans++;
			else {
				state->last_dir = dir;
				state->last_seq = seq;
				state->last_ack = ack;
				state->last_end = end;
				state->retrans = 0;
			}
		}
		/*
		 * Close the window of disabled window tracking :-)
		 */
		if (sender->loose)
			sender->loose--;
		
		res = 1;
	} else {
		if (LOG_INVALID(IPPROTO_TCP))
			nf_log_packet(PF_INET, 0, skb, NULL, NULL, NULL,
			"ip_ct_tcp: %s ",
			before(seq, sender->td_maxend + 1) ?
			after(end, sender->td_end - receiver->td_maxwin - 1) ?
			before(sack, receiver->td_end + 1) ?
			after(ack, receiver->td_end - MAXACKWINDOW(sender)) ? "BUG"
			: "ACK is under the lower bound (possible overly delayed ACK)"
			: "ACK is over the upper bound (ACKed data not seen yet)"
			: "SEQ is under the lower bound (already ACKed data retransmitted)"
			: "SEQ is over the upper bound (over the window of the receiver)");

		res = ip_ct_tcp_be_liberal;
  	}
  
	DEBUGP("tcp_in_window: res=%i sender end=%u maxend=%u maxwin=%u "
	       "receiver end=%u maxend=%u maxwin=%u\n",
		res, sender->td_end, sender->td_maxend, sender->td_maxwin, 
		receiver->td_end, receiver->td_maxend, receiver->td_maxwin);

	return res;
}

#ifdef CONFIG_IP_NF_NAT_NEEDED
/* Update sender->td_end after NAT successfully mangled the packet */
void ip_conntrack_tcp_update(struct sk_buff *skb,
			     struct ip_conntrack *conntrack, 
			     enum ip_conntrack_dir dir)
{
	struct iphdr *iph = skb->nh.iph;
	struct tcphdr *tcph = (void *)skb->nh.iph + skb->nh.iph->ihl*4;
	__u32 end;
#ifdef DEBUGP_VARS
	struct ip_ct_tcp_state *sender = &conntrack->proto.tcp.seen[dir];
	struct ip_ct_tcp_state *receiver = &conntrack->proto.tcp.seen[!dir];
#endif

	end = segment_seq_plus_len(ntohl(tcph->seq), skb->len, iph, tcph);
	
	write_lock_bh(&tcp_lock);
	/*
	 * We have to worry for the ack in the reply packet only...
	 */
	if (after(end, conntrack->proto.tcp.seen[dir].td_end))
		conntrack->proto.tcp.seen[dir].td_end = end;
	conntrack->proto.tcp.last_end = end;
	write_unlock_bh(&tcp_lock);
	DEBUGP("tcp_update: sender end=%u maxend=%u maxwin=%u scale=%i "
	       "receiver end=%u maxend=%u maxwin=%u scale=%i\n",
		sender->td_end, sender->td_maxend, sender->td_maxwin,
		sender->td_scale, 
		receiver->td_end, receiver->td_maxend, receiver->td_maxwin,
		receiver->td_scale);
}
 
#endif

#define	TH_FIN	0x01
#define	TH_SYN	0x02
#define	TH_RST	0x04
#define	TH_PUSH	0x08
#define	TH_ACK	0x10
#define	TH_URG	0x20
#define	TH_ECE	0x40
#define	TH_CWR	0x80

/* table of valid flag combinations - ECE and CWR are always valid */
static const u8 tcp_valid_flags[(TH_FIN|TH_SYN|TH_RST|TH_PUSH|TH_ACK|TH_URG) + 1] =
{
	[TH_SYN]			= 1,
	[TH_SYN|TH_ACK]			= 1,
	[TH_SYN|TH_PUSH]		= 1,
	[TH_SYN|TH_ACK|TH_PUSH]		= 1,
	[TH_RST]			= 1,
	[TH_RST|TH_ACK]			= 1,
	[TH_RST|TH_ACK|TH_PUSH]		= 1,
	[TH_FIN|TH_ACK]			= 1,
	[TH_ACK]			= 1,
	[TH_ACK|TH_PUSH]		= 1,
	[TH_ACK|TH_URG]			= 1,
	[TH_ACK|TH_URG|TH_PUSH]		= 1,
	[TH_FIN|TH_ACK|TH_PUSH]		= 1,
	[TH_FIN|TH_ACK|TH_URG]		= 1,
	[TH_FIN|TH_ACK|TH_URG|TH_PUSH]	= 1,
};

/* Protect conntrack agaist broken packets. Code taken from ipt_unclean.c.  */
static int tcp_error(struct sk_buff *skb,
		     enum ip_conntrack_info *ctinfo,
		     unsigned int hooknum)
{
	struct iphdr *iph = skb->nh.iph;
	struct tcphdr _tcph, *th;
	unsigned int tcplen = skb->len - iph->ihl * 4;
	u_int8_t tcpflags;

	/* Smaller that minimal TCP header? */
	th = skb_header_pointer(skb, iph->ihl * 4,
				sizeof(_tcph), &_tcph);
	if (th == NULL) {
		if (LOG_INVALID(IPPROTO_TCP))
			nf_log_packet(PF_INET, 0, skb, NULL, NULL, NULL,
				"ip_ct_tcp: short packet ");
		return -NF_ACCEPT;
  	}
  
	/* Not whole TCP header or malformed packet */
	if (th->doff*4 < sizeof(struct tcphdr) || tcplen < th->doff*4) {
		if (LOG_INVALID(IPPROTO_TCP))
			nf_log_packet(PF_INET, 0, skb, NULL, NULL, NULL,
				"ip_ct_tcp: truncated/malformed packet ");
		return -NF_ACCEPT;
	}
  
	/* Checksum invalid? Ignore.
	 * We skip checking packets on the outgoing path
	 * because the semantic of CHECKSUM_HW is different there 
	 * and moreover root might send raw packets.
	 */
	/* FIXME: Source route IP option packets --RR */
	if (hooknum == NF_IP_PRE_ROUTING
	    && skb->ip_summed != CHECKSUM_UNNECESSARY
	    && csum_tcpudp_magic(iph->saddr, iph->daddr, tcplen, IPPROTO_TCP,
			         skb->ip_summed == CHECKSUM_HW ? skb->csum
			      	 : skb_checksum(skb, iph->ihl*4, tcplen, 0))) {
		if (LOG_INVALID(IPPROTO_TCP))
			nf_log_packet(PF_INET, 0, skb, NULL, NULL, NULL,
				  "ip_ct_tcp: bad TCP checksum ");
		return -NF_ACCEPT;
	}

	/* Check TCP flags. */
	tcpflags = (((u_int8_t *)th)[13] & ~(TH_ECE|TH_CWR));
	if (!tcp_valid_flags[tcpflags]) {
		if (LOG_INVALID(IPPROTO_TCP))
			nf_log_packet(PF_INET, 0, skb, NULL, NULL, NULL,
				  "ip_ct_tcp: invalid TCP flag combination ");
		return -NF_ACCEPT;
	}

	return NF_ACCEPT;
}

/* Returns verdict for packet, or -1 for invalid. */
static int tcp_packet(struct ip_conntrack *conntrack,
		      const struct sk_buff *skb,
		      enum ip_conntrack_info ctinfo)
{
	enum tcp_conntrack new_state, old_state;
	enum ip_conntrack_dir dir;
	struct iphdr *iph = skb->nh.iph;
	struct tcphdr *th, _tcph;
	unsigned long timeout;
	unsigned int index;
	
	th = skb_header_pointer(skb, iph->ihl * 4,
				sizeof(_tcph), &_tcph);
	BUG_ON(th == NULL);
	
	write_lock_bh(&tcp_lock);
	old_state = conntrack->proto.tcp.state;
	dir = CTINFO2DIR(ctinfo);
	index = get_conntrack_index(th);
	new_state = tcp_conntracks[dir][index][old_state];

	switch (new_state) {
	case TCP_CONNTRACK_IGNORE:
		/* Either SYN in ORIGINAL
		 * or SYN/ACK in REPLY. */
		if (index == TCP_SYNACK_SET
		    && conntrack->proto.tcp.last_index == TCP_SYN_SET
		    && conntrack->proto.tcp.last_dir != dir
		    && ntohl(th->ack_seq) ==
		    	     conntrack->proto.tcp.last_end) {
			/* This SYN/ACK acknowledges a SYN that we earlier 
			 * ignored as invalid. This means that the client and
			 * the server are both in sync, while the firewall is
			 * not. We kill this session and block the SYN/ACK so
			 * that the client cannot but retransmit its SYN and 
			 * thus initiate a clean new session.
			 */
		    	write_unlock_bh(&tcp_lock);
			if (LOG_INVALID(IPPROTO_TCP))
				nf_log_packet(PF_INET, 0, skb, NULL, NULL,
					      NULL, "ip_ct_tcp: "
					      "killing out of sync session ");
		    	if (del_timer(&conntrack->timeout))
		    		conntrack->timeout.function((unsigned long)
		    					    conntrack);
		    	return -NF_DROP;
		}
		conntrack->proto.tcp.last_index = index;
		conntrack->proto.tcp.last_dir = dir;
		conntrack->proto.tcp.last_seq = ntohl(th->seq);
		conntrack->proto.tcp.last_end = 
		    segment_seq_plus_len(ntohl(th->seq), skb->len, iph, th);
		
		write_unlock_bh(&tcp_lock);
		if (LOG_INVALID(IPPROTO_TCP))
			nf_log_packet(PF_INET, 0, skb, NULL, NULL, NULL,
				  "ip_ct_tcp: invalid packet ignored ");
		return NF_ACCEPT;
	case TCP_CONNTRACK_MAX:
		/* Invalid packet */
		DEBUGP("ip_ct_tcp: Invalid dir=%i index=%u ostate=%u\n",
		       dir, get_conntrack_index(th),
		       old_state);
		write_unlock_bh(&tcp_lock);
		if (LOG_INVALID(IPPROTO_TCP))
			nf_log_packet(PF_INET, 0, skb, NULL, NULL, NULL,
				  "ip_ct_tcp: invalid state ");
		return -NF_ACCEPT;
	case TCP_CONNTRACK_SYN_SENT:
		if (old_state < TCP_CONNTRACK_TIME_WAIT)
			break;
		if ((conntrack->proto.tcp.seen[dir].flags &
		         IP_CT_TCP_FLAG_CLOSE_INIT)
		    || after(ntohl(th->seq),
		    	     conntrack->proto.tcp.seen[dir].td_end)) {	
		    	/* Attempt to reopen a closed connection.
		    	* Delete this connection and look up again. */
		    	write_unlock_bh(&tcp_lock);
		    	if (del_timer(&conntrack->timeout))
		    		conntrack->timeout.function((unsigned long)
		    					    conntrack);
		    	return -NF_REPEAT;
		} else {
			write_unlock_bh(&tcp_lock);
			if (LOG_INVALID(IPPROTO_TCP))
				nf_log_packet(PF_INET, 0, skb, NULL, NULL,
					      NULL, "ip_ct_tcp: invalid SYN");
			return -NF_ACCEPT;
		}
	case TCP_CONNTRACK_CLOSE:
		if (index == TCP_RST_SET
		    && test_bit(IPS_SEEN_REPLY_BIT, &conntrack->status)
		    && conntrack->proto.tcp.last_index == TCP_SYN_SET
		    && ntohl(th->ack_seq) == conntrack->proto.tcp.last_end) {
			/* RST sent to invalid SYN we had let trough
			 * SYN was in window then, tear down connection.
			 * We skip window checking, because packet might ACK
			 * segments we ignored in the SYN. */
			goto in_window;
		}
		/* Just fall trough */
	default:
		/* Keep compilers happy. */
		break;
	}

	if (!tcp_in_window(&conntrack->proto.tcp, dir, index, 
			   skb, iph, th)) {
		write_unlock_bh(&tcp_lock);
		return -NF_ACCEPT;
	}
    in_window:
	/* From now on we have got in-window packets */	
	conntrack->proto.tcp.last_index = index;

	DEBUGP("tcp_conntracks: src=%u.%u.%u.%u:%hu dst=%u.%u.%u.%u:%hu "
	       "syn=%i ack=%i fin=%i rst=%i old=%i new=%i\n",
		NIPQUAD(iph->saddr), ntohs(th->source),
		NIPQUAD(iph->daddr), ntohs(th->dest),
		(th->syn ? 1 : 0), (th->ack ? 1 : 0),
		(th->fin ? 1 : 0), (th->rst ? 1 : 0),
		old_state, new_state);

	conntrack->proto.tcp.state = new_state;
	if (old_state != new_state 
	    && (new_state == TCP_CONNTRACK_FIN_WAIT
	    	|| new_state == TCP_CONNTRACK_CLOSE))
		conntrack->proto.tcp.seen[dir].flags |= IP_CT_TCP_FLAG_CLOSE_INIT;
	timeout = conntrack->proto.tcp.retrans >= ip_ct_tcp_max_retrans
		  && *tcp_timeouts[new_state] > ip_ct_tcp_timeout_max_retrans
		  ? ip_ct_tcp_timeout_max_retrans : *tcp_timeouts[new_state];
	write_unlock_bh(&tcp_lock);

	ip_conntrack_event_cache(IPCT_PROTOINFO_VOLATILE, skb);
	if (new_state != old_state)
		ip_conntrack_event_cache(IPCT_PROTOINFO, skb);

	if (!test_bit(IPS_SEEN_REPLY_BIT, &conntrack->status)) {
		/* If only reply is a RST, we can consider ourselves not to
		   have an established connection: this is a fairly common
		   problem case, so we can delete the conntrack
		   immediately.  --RR */
		if (th->rst) {
			if (del_timer(&conntrack->timeout))
				conntrack->timeout.function((unsigned long)
							    conntrack);
			return NF_ACCEPT;
		}
	} else if (!test_bit(IPS_ASSURED_BIT, &conntrack->status)
		   && (old_state == TCP_CONNTRACK_SYN_RECV
		       || old_state == TCP_CONNTRACK_ESTABLISHED)
		   && new_state == TCP_CONNTRACK_ESTABLISHED) {
		/* Set ASSURED if we see see valid ack in ESTABLISHED 
		   after SYN_RECV or a valid answer for a picked up 
		   connection. */
		set_bit(IPS_ASSURED_BIT, &conntrack->status);
		ip_conntrack_event_cache(IPCT_STATUS, skb);
	}
	ip_ct_refresh_acct(conntrack, ctinfo, skb, timeout);

	return NF_ACCEPT;
}
 
/* Called when a new connection for this protocol found. */
static int tcp_new(struct ip_conntrack *conntrack,
		   const struct sk_buff *skb)
{
	enum tcp_conntrack new_state;
	struct iphdr *iph = skb->nh.iph;
	struct tcphdr *th, _tcph;
#ifdef DEBUGP_VARS
	struct ip_ct_tcp_state *sender = &conntrack->proto.tcp.seen[0];
	struct ip_ct_tcp_state *receiver = &conntrack->proto.tcp.seen[1];
#endif

	th = skb_header_pointer(skb, iph->ihl * 4,
				sizeof(_tcph), &_tcph);
	BUG_ON(th == NULL);
	
	/* Don't need lock here: this conntrack not in circulation yet */
	new_state
		= tcp_conntracks[0][get_conntrack_index(th)]
		[TCP_CONNTRACK_NONE];

	/* Invalid: delete conntrack */
	if (new_state >= TCP_CONNTRACK_MAX) {
		DEBUGP("ip_ct_tcp: invalid new deleting.\n");
		return 0;
	}

	if (new_state == TCP_CONNTRACK_SYN_SENT) {
		/* SYN packet */
		conntrack->proto.tcp.seen[0].td_end =
			segment_seq_plus_len(ntohl(th->seq), skb->len,
					     iph, th);
		conntrack->proto.tcp.seen[0].td_maxwin = ntohs(th->window);
		if (conntrack->proto.tcp.seen[0].td_maxwin == 0)
			conntrack->proto.tcp.seen[0].td_maxwin = 1;
		conntrack->proto.tcp.seen[0].td_maxend =
			conntrack->proto.tcp.seen[0].td_end;

		tcp_options(skb, iph, th, &conntrack->proto.tcp.seen[0]);
		conntrack->proto.tcp.seen[1].flags = 0;
		conntrack->proto.tcp.seen[0].loose = 
		conntrack->proto.tcp.seen[1].loose = 0;
	} else if (ip_ct_tcp_loose == 0) {
		/* Don't try to pick up connections. */
		return 0;
	} else {
		/*
		 * We are in the middle of a connection,
		 * its history is lost for us.
		 * Let's try to use the data from the packet.
		 */
		conntrack->proto.tcp.seen[0].td_end =
			segment_seq_plus_len(ntohl(th->seq), skb->len,
					     iph, th);
		conntrack->proto.tcp.seen[0].td_maxwin = ntohs(th->window);
		if (conntrack->proto.tcp.seen[0].td_maxwin == 0)
			conntrack->proto.tcp.seen[0].td_maxwin = 1;
		conntrack->proto.tcp.seen[0].td_maxend =
			conntrack->proto.tcp.seen[0].td_end + 
			conntrack->proto.tcp.seen[0].td_maxwin;
		conntrack->proto.tcp.seen[0].td_scale = 0;

		/* We assume SACK. Should we assume window scaling too? */
		conntrack->proto.tcp.seen[0].flags =
		conntrack->proto.tcp.seen[1].flags = IP_CT_TCP_FLAG_SACK_PERM;
		conntrack->proto.tcp.seen[0].loose = 
		conntrack->proto.tcp.seen[1].loose = ip_ct_tcp_loose;
	}
    
	conntrack->proto.tcp.seen[1].td_end = 0;
	conntrack->proto.tcp.seen[1].td_maxend = 0;
	conntrack->proto.tcp.seen[1].td_maxwin = 1;
	conntrack->proto.tcp.seen[1].td_scale = 0;      

	/* tcp_packet will set them */
	conntrack->proto.tcp.state = TCP_CONNTRACK_NONE;
	conntrack->proto.tcp.last_index = TCP_NONE_SET;
	 
	DEBUGP("tcp_new: sender end=%u maxend=%u maxwin=%u scale=%i "
	       "receiver end=%u maxend=%u maxwin=%u scale=%i\n",
		sender->td_end, sender->td_maxend, sender->td_maxwin,
		sender->td_scale, 
		receiver->td_end, receiver->td_maxend, receiver->td_maxwin,
		receiver->td_scale);
	return 1;
}
  
struct ip_conntrack_protocol ip_conntrack_protocol_tcp =
{
	.proto 			= IPPROTO_TCP,
	.name 			= "tcp",
	.pkt_to_tuple 		= tcp_pkt_to_tuple,
	.invert_tuple 		= tcp_invert_tuple,
	.print_tuple 		= tcp_print_tuple,
	.print_conntrack 	= tcp_print_conntrack,
	.packet 		= tcp_packet,
	.new 			= tcp_new,
	.error			= tcp_error,
#if defined(CONFIG_IP_NF_CONNTRACK_NETLINK) || \
    defined(CONFIG_IP_NF_CONNTRACK_NETLINK_MODULE)
	.to_nfattr		= tcp_to_nfattr,
	.from_nfattr		= nfattr_to_tcp,
	.tuple_to_nfattr	= ip_ct_port_tuple_to_nfattr,
	.nfattr_to_tuple	= ip_ct_port_nfattr_to_tuple,
#endif
};
