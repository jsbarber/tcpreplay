/* $Id: flownode.c,v 1.1 2003/05/29 21:58:12 aturner Exp $ */

/*
 * Copyright (c) 2003 Aaron Turner.
 * All rights reserved.
 *
 * Please see Docs/LICENSE for licensing information
 */

#include "flowreplay.h"
#include "flownode.h"
#include "flowkey.h"
#include "err.h"

extern struct session_tree tcproot, udproot;
extern int nfds;
extern struct in_addr targetaddr;

/* prepare the RB trees for tcp and udp sessions */
RB_PROTOTYPE(session_tree, session_t, node, rbsession_comp)
RB_GENERATE(session_tree, session_t, node, rbsession_comp)


/*
 * returns the session_t structure
 * based upon the key given for the RB root (one root per
 * protocol).  If the key doesn't exist, it will return NULL
 *
 * NOTE: This function is broken!  key's are not guaranteed
 * to be unique for all combinations of sessions.  What we
 * really should be doing is using a rbtree using a 32bit
 * key and then solving for collisions via a linked list.
 * this would probably be faster for the common case and still
 * provide adequate speed for collisions rather then ignoring
 * the collsion problem all together.
 */
struct session_t *
getnodebykey(char proto, u_int64_t key)
{
    struct session_t *node = NULL;
    struct session_t like;

    like.socket = -1;
    like.key = key;

    if (proto == IPPROTO_TCP) {
	if ((node = RB_FIND(session_tree, &tcproot, &like)) == NULL) {
	    dbg(3, "Couldn't find TCP key: 0x%llx", key);
	    return(NULL);
	}
    } 

    else if (proto == IPPROTO_UDP) {
	if ((node = RB_FIND(session_tree, &udproot, &like)) == NULL) {
	    dbg(3, "Couldn't find UDP key: 0x%llx", key);
	    return(NULL);
	}
    } 

    else {
	warnx("Invalid tree protocol: 0x%x", proto);
	return(NULL);
    }

    dbg(3, "Found 0x%llx in the tree", key);
    return(node);

}

/*
 * inserts a node into a tree.
 * we fill out the node and create a new open socket 
 * we then return the node or NULL on error
 */
struct session_t *
newnode(char proto, u_int64_t key, ip_hdr_t *ip_hdr, void *l4)
{
    struct sockaddr_in sa;
    struct session_t *newnode = NULL;
    const int on = 1;
    tcp_hdr_t *tcp_hdr = NULL;
    udp_hdr_t *udp_hdr = NULL;


    dbg(2, "Adding new node: 0x%llx", key);

    if ((newnode = (struct session_t *)malloc(sizeof(struct session_t))) == NULL)
	errx(1, "Unable to malloc memory for a new node");

    memset(newnode, '\0', sizeof(struct session_t));

    newnode->key = key;

    newnode->proto = ip_hdr->ip_p;

    /* create a TCP or UDP socket & insert it in the tree */
    if (newnode->proto == IPPROTO_TCP) {
	/* is this a Syn packet? */
	tcp_hdr = (tcp_hdr_t *)l4;
	if ( tcp_hdr->th_flags != TH_SYN) {
	    free(newnode);
	    warnx("We won't connect (%s:%d -> %s:%d) on non-Syn packets", 
		  libnet_addr2name4(ip_hdr->ip_src.s_addr, LIBNET_DONT_RESOLVE),
		  ntohs(tcp_hdr->th_sport),
		  libnet_addr2name4(ip_hdr->ip_dst.s_addr, LIBNET_DONT_RESOLVE),
		  ntohs(tcp_hdr->th_dport));
	    return(NULL);
	}

	/* otherwise, continue on our merry way */
	newnode->server_ip = ip_hdr->ip_dst.s_addr;
	newnode->server_port = tcp_hdr->th_dport;
	newnode->state = TH_SYN;
	newnode->direction = C2S;
	newnode->wait = DONT_WAIT;

	if ((newnode->socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
	    free(newnode);
	    warnx("Unable to create new TCP socket: %s", strerror(errno));
	    return(NULL);
	}

	/* make our socket reusable */
	setsockopt(newnode->socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	RB_INSERT(session_tree, &tcproot, newnode);
	sa.sin_port = tcp_hdr->th_dport;
    } 

    else if (newnode->proto == IPPROTO_UDP) {
	udp_hdr = (udp_hdr_t *)l4;
	/* 
	 * we're not as smart about UDP as TCP so we just assume
	 * the first UDP packet is client->server
	 */
	newnode->server_ip = ip_hdr->ip_dst.s_addr;
	newnode->server_port = udp_hdr->uh_dport;
	newnode->direction = C2S;
	newnode->wait = DONT_WAIT;

	if ((newnode->socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
	    free(newnode);
	    warnx("Unable to create new UDP socket: %s", strerror(errno));
	    return(NULL);
	}

	/* make our socket reusable */
	setsockopt(newnode->socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	RB_INSERT(session_tree, &udproot, newnode);
	sa.sin_port = udp_hdr->uh_dport;
    }

    /* connect to socket */
    sa.sin_family = AF_INET;

    /* set the appropriate destination IP */
    if (targetaddr.s_addr != 0) {
	sa.sin_addr = targetaddr;
    } else {
	sa.sin_addr = ip_hdr->ip_dst;
    }

    if (connect(newnode->socket, (struct sockaddr *)&sa, sizeof(struct sockaddr_in)) < 0) {
	free(newnode);
	warnx("Unable to connect to %s:%hu: %s", inet_ntoa(sa.sin_addr), 
	     ntohs(sa.sin_port), strerror(errno));
	return(NULL);
    }

    dbg(2, "Connected to %s:%hu as socketID: %d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), 
	newnode->socket);

    /* increment nfds so our select() works */
    if (nfds <= newnode->socket)
	nfds = newnode->socket + 1;

    return(newnode);
}

/*
 * compare two session_t structs for the RB_TREE compare
 */
int 
rbsession_comp(struct session_t *a, struct session_t *b)
{
    if (a->key < b->key) return (-1);
    else if (a->key > b->key) return (1);
    return (0);
}

/*
 * A wrapper around RB_REMOVE to delete a node from a tree
 */

void
delete_node(struct session_tree *root , struct session_t *node)
{
    dbg(2, "Deleting node 0x%llx", node->key);
    RB_REMOVE(session_tree, root, node);
}


void
close_sockets(void)
{
    int tcpcount = 0, udpcount = 0;
    struct session_t *node = NULL;

    /* close the TCP sockets */
    RB_FOREACH(node, session_tree, &tcproot) {
	close(node->socket);
	tcpcount ++;
    }

    /* close the UDP sockets */
    RB_FOREACH(node, session_tree, &udproot) {
	close(node->socket);
	udpcount ++;
    }
    dbg(1, "Closed %d tcp and %d udp socket(s)", tcpcount, udpcount);
}
