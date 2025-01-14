/*	$OpenBSD: pfvar_priv.h,v 1.9 2022/04/08 18:17:24 bluhm Exp $	*/

/*
 * Copyright (c) 2001 Daniel Hartmeier
 * Copyright (c) 2002 - 2013 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 2016 Alexander Bluhm <bluhm@openbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    - Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _NET_PFVAR_PRIV_H_
#define _NET_PFVAR_PRIV_H_

#ifdef _KERNEL

#include <sys/rwlock.h>
#include <sys/mutex.h>

/*
 *
 * states are linked into a global list to support the following
 * functionality:
 *
 * - garbage collection
 * - pfsync bulk send operations
 * - bulk state fetches via the DIOCGETSTATES ioctl
 * - bulk state clearing via the DIOCCLRSTATES ioctl
 * 
 * states are inserted into the global pf_state_list once it has also
 * been successfully added to the various trees that make up the state
 * table. states are only removed from the pf_state_list by the garbage
 * collection process.

 * the pf_state_list head and tail pointers (ie, the pfs_list TAILQ_HEAD
 * structure) and the pointers between the entries on the pf_state_list
 * are locked separately. at a high level, this allows for insertion
 * of new states into the pf_state_list while other contexts (eg, the
 * ioctls) are traversing the state items in the list. for garbage
 * collection to remove items from the pf_state_list, it has to exclude
 * both modifications to the list head and tail pointers, and traversal
 * of the links between the states.
 *
 * the head and tail pointers are protected by a mutex. the pointers
 * between states are protected by an rwlock.
 *
 * because insertions are only made to the end of the list, if we get
 * a snapshot of the head and tail of the list and prevent modifications
 * to the links between states, we can safely traverse between the
 * head and tail entries. subsequent insertions can add entries after
 * our view of the tail, but we don't look past our view.
 *
 * if both locks must be taken, the rwlock protecting the links between
 * states is taken before the mutex protecting the head and tail
 * pointer.
 *
 * insertion into the list follows this pattern:
 *
 *	// serialise list head/tail modifications
 *	mtx_enter(&pf_state_list.pfs_mtx);
 *	TAILQ_INSERT_TAIL(&pf_state_list.pfs_list, state, entry_list);
 *	mtx_leave(&pf_state_list.pfs_mtx);
 *
 * traversal of the list:
 *
 *	// lock against the gc removing an item from the list
 *	rw_enter_read(&pf_state_list.pfs_rwl);
 *
 *	// get a snapshot view of the ends of the list
 *	mtx_enter(&pf_state_list.pfs_mtx);
 *	head = TAILQ_FIRST(&pf_state_list.pfs_list);
 *	tail = TAILQ_LAST(&pf_state_list.pfs_list, pf_state_queue);
 *	mtx_leave(&pf_state_list.pfs_mtx);
 *
 *	state = NULL;
 *	next = head;
 *
 *	while (state != tail) {
 *		state = next;
 *		next = TAILQ_NEXT(state, entry_list);
 *
 *		// look at the state
 *	}
 *
 *	rw_exit_read(&pf_state_list.pfs_rwl);
 *
 * removing an item from the list:
 * 
 *	// wait for iterators (readers) to get out
 *	rw_enter_write(&pf_state_list.pfs_rwl);
 *
 *	// serialise list head/tail modifications
 *	mtx_enter(&pf_state_list.pfs_mtx);
 *	TAILQ_REMOVE(&pf_state_list.pfs_list, state, entry_list);
 *	mtx_leave(&pf_state_list.pfs_mtx);
 *
 *	rw_exit_write(&pf_state_list.pfs_rwl);
 *
 * the lock ordering for pf_state_list locks and the rest of the pf
 * locks are:
 *
 * 1. KERNEL_LOCK
 * 2. NET_LOCK
 * 3. pf_state_list.pfs_rwl
 * 4. PF_LOCK
 * 5. PF_STATE_LOCK
 * 6. pf_state_list.pfs_mtx
 */

struct pf_state_list {
	/* the list of states in the system */
	struct pf_state_queue		pfs_list;

	/* serialise pfs_list head/tail access */
	struct mutex			pfs_mtx;

	/* serialise access to pointers between pfs_list entries */
	struct rwlock			pfs_rwl;
};

#define PF_STATE_LIST_INITIALIZER(_pfs) {				\
	.pfs_list	= TAILQ_HEAD_INITIALIZER(_pfs.pfs_list),	\
	.pfs_mtx	= MUTEX_INITIALIZER(IPL_SOFTNET),		\
	.pfs_rwl	= RWLOCK_INITIALIZER("pfstates"),		\
}

extern struct rwlock pf_lock;

struct pf_pdesc {
	struct {
		int	 done;
		uid_t	 uid;
		gid_t	 gid;
		pid_t	 pid;
	}		 lookup;
	u_int64_t	 tot_len;	/* Make Mickey money */

	struct pf_addr	 nsaddr;	/* src address after NAT */
	struct pf_addr	 ndaddr;	/* dst address after NAT */

	struct pfi_kif	*kif;		/* incoming interface */
	struct mbuf	*m;		/* mbuf containing the packet */
	struct pf_addr	*src;		/* src address */
	struct pf_addr	*dst;		/* dst address */
	u_int16_t	*pcksum;	/* proto cksum */
	u_int16_t	*sport;
	u_int16_t	*dport;
	u_int16_t	 osport;
	u_int16_t	 odport;
	u_int16_t	 nsport;	/* src port after NAT */
	u_int16_t	 ndport;	/* dst port after NAT */

	u_int32_t	 off;		/* protocol header offset */
	u_int32_t	 hdrlen;	/* protocol header length */
	u_int32_t	 p_len;		/* length of protocol payload */
	u_int32_t	 extoff;	/* extension header offset */
	u_int32_t	 fragoff;	/* fragment header offset */
	u_int32_t	 jumbolen;	/* length from v6 jumbo header */
	u_int32_t	 badopts;	/* v4 options or v6 routing headers */

	u_int16_t	 rdomain;	/* original routing domain */
	u_int16_t	 virtual_proto;
#define PF_VPROTO_FRAGMENT	256
	sa_family_t	 af;
	sa_family_t	 naf;
	u_int8_t	 proto;
	u_int8_t	 tos;
	u_int8_t	 ttl;
	u_int8_t	 dir;		/* direction */
	u_int8_t	 sidx;		/* key index for source */
	u_int8_t	 didx;		/* key index for destination */
	u_int8_t	 destchg;	/* flag set when destination changed */
	u_int8_t	 pflog;		/* flags for packet logging */
	union {
		struct tcphdr			tcp;
		struct udphdr			udp;
		struct icmp			icmp;
#ifdef INET6
		struct icmp6_hdr		icmp6;
		struct mld_hdr			mld;
		struct nd_neighbor_solicit	nd_ns;
#endif /* INET6 */
	} hdr;
};

extern struct task	pf_purge_task;
extern struct timeout	pf_purge_to;

struct pf_state		*pf_state_ref(struct pf_state *);
void			 pf_state_unref(struct pf_state *);

extern struct rwlock	pf_lock;
extern struct rwlock	pf_state_lock;

#define PF_LOCK()		do {			\
		NET_ASSERT_LOCKED();			\
		rw_enter_write(&pf_lock);		\
	} while (0)

#define PF_UNLOCK()		do {			\
		PF_ASSERT_LOCKED();			\
		rw_exit_write(&pf_lock);		\
	} while (0)

#define PF_ASSERT_LOCKED()	do {			\
		if (rw_status(&pf_lock) != RW_WRITE)	\
			splassert_fail(RW_WRITE,	\
			    rw_status(&pf_lock),__func__);\
	} while (0)

#define PF_ASSERT_UNLOCKED()	do {			\
		if (rw_status(&pf_lock) == RW_WRITE)	\
			splassert_fail(0, rw_status(&pf_lock), __func__);\
	} while (0)

#define PF_STATE_ENTER_READ()	do {			\
		rw_enter_read(&pf_state_lock);		\
	} while (0)

#define PF_STATE_EXIT_READ()	do {			\
		rw_exit_read(&pf_state_lock);		\
	} while (0)

#define PF_STATE_ENTER_WRITE()	do {			\
		rw_enter_write(&pf_state_lock);		\
	} while (0)

#define PF_STATE_EXIT_WRITE()	do {			\
		PF_STATE_ASSERT_LOCKED();		\
		rw_exit_write(&pf_state_lock);		\
	} while (0)

#define PF_STATE_ASSERT_LOCKED()	do {		\
		if (rw_status(&pf_state_lock) != RW_WRITE)\
			splassert_fail(RW_WRITE,	\
			    rw_status(&pf_state_lock), __func__);\
	} while (0)

extern void			 pf_purge_timeout(void *);
extern void			 pf_purge(void *);
#endif /* _KERNEL */

#endif /* _NET_PFVAR_PRIV_H_ */
