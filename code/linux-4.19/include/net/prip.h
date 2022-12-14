#ifndef _PRIP_H
#define _PRIP_H

#include <asm/atomic.h>
#include <linux/spinlock.h>
#include <net/sock.h>
#include <linux/list.h>
#include <linux/timer.h>

#ifndef CONFIG_PRIP
#define CONFIG_PRIP
#endif

#ifndef PRIP_HASHSZ
#define PRIP_HASHSZ 64
#endif

struct prip_hash_list{
    struct hlist_head head;
    rwlock_t rwlock;
};
extern struct prip_hash_list prip_hash[PRIP_HASHSZ];
extern struct prip_hash_list prip_single_list;
extern atomic_t prip_netmask;
extern __u32 prip_net_one;
extern __u32 prip_net_two;

typedef struct prip {
	unsigned char valid:1, /*Determine if the structure is valid*/
			net_one_flag:1,/* 1:master net ; 0:slave net*/
			net_two_flag:1;/* 1:master net ; 0:slave net*/
	unsigned int net_one;/*the first network number set by the user*/
	unsigned int net_two;/*the second network number set by the user*/
    	unsigned int mask; /*netmask set by usr*/
   	/* unsigned int master;master network set by setsockopt API.
    	unsigned int slave;slave network*/
    	rwlock_t rwlock;
    	atomic_t  master_sure;/*  >=1: sure master net; 0:not sure master*/
    	atomic_t  reference; /*the reference count*/
} PRIP_CONFIG_T;

typedef union ip_addr{
    unsigned int ip;
    unsigned char buff[4];
} IP_ADDR;

struct link_entry{
	char *seq_in_top;
	char *seq_in_bottom;
	unsigned long *clear_map;
	int lostcount[2];
};
struct prip_priv{
	struct link_entry *link;
	__u16 sequence_nr_out;
	spinlock_t seqnr_out_lock;
	spinlock_t seqnr_in_lock;
    spinlock_t alarm_lock;
    spinlock_t timer_lock;
    atomic_t refcnt;
	__u32 peerip;
	__u32 localip;
    __u32  peer_slave_ip;
	unsigned long snd_start;
    struct prip_hash_list *prip_hash_head;
    struct prip_hash_list *prip_single_head;
    struct hlist_node hash_list;
    struct hlist_node single_list;
    atomic_t  master_status;/* 1: up status ; 0: down status*/
    atomic_t  slave_status;/*  1: up status ; 0: down status*/
    atomic64_t master_send_num;
    atomic64_t slave_send_num;
    atomic64_t master_recv_num;
    atomic64_t slave_recv_num;
    struct timer_list timer;
};

extern int set_prip_mode(struct sock *sk, int mode);
extern __u32 master_to_slave(__u32 master_ip);
extern __u32 slave_to_master(__u32 slave_ip);
extern void set_slave_stat(struct prip_priv *q , __u32 stat);
extern void set_master_stat(struct prip_priv *q , __u32 stat);
extern __u32  get_master_or_slave(int flag);
extern void master_send_inc(struct prip_priv *q);
extern void master_recv_inc(struct prip_priv *q);
extern void slave_send_inc(struct prip_priv *q);
extern void slave_recv_inc(struct prip_priv *q);
extern int get_prip_alarm(void);
extern void prip_priv_put(struct prip_priv *q);
extern void prip_priv_timeout(struct timer_list *t);
extern struct prip_priv *prip_priv_find(__u32 localip, __u32 peerip);
extern struct prip_priv *prip_priv_only_find(__u32 localip, __u32 peerip);
extern  PRIP_CONFIG_T prip_config;
extern  bool prip_isinited;
extern  atomic_t prip_alarm;
extern  struct proc_dir_entry *dir_prip;
extern  struct proc_dir_entry *prip_config_entry;
extern  struct proc_dir_entry *prip_status_entry;
extern  struct proc_dir_entry *prip_alarm_entry;

extern struct prip_hash_list prip_hash[PRIP_HASHSZ];
extern struct prip_hash_list prip_single_list;
extern int prip_check(u16 seq, int isdup, struct prip_priv *priv, unsigned long start);
extern  u16 get_pripid(struct prip_priv *priv, unsigned long *snd_start);
extern int check_prip_net(__u32 addr);

#endif
