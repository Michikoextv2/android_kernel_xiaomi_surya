/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_BORE_H
#define _LINUX_SCHED_BORE_H

#include <linux/sched.h>

#define SCHED_BORE_VERSION "5.9.6-CFS-4.14"

#ifdef CONFIG_SCHED_BORE
extern unsigned int sched_bore;
extern unsigned int sched_burst_exclude_kthreads;
extern unsigned int sched_burst_smoothness_long;
extern unsigned int sched_burst_smoothness_short;
extern unsigned int sched_burst_fork_atavistic;
extern unsigned int sched_burst_parity_threshold;
extern unsigned int sched_burst_penalty_offset;
extern unsigned int sched_burst_penalty_scale;
extern unsigned int sched_burst_cache_stop_count;
extern unsigned int sched_burst_cache_lifetime;

extern void update_burst_score(struct sched_entity *se);
extern void update_burst_penalty(struct sched_entity *se);
extern void restart_burst(struct sched_entity *se);
extern void reset_task_bore(struct task_struct *p);

extern void sched_clone_bore(struct task_struct *p,
			     struct task_struct *parent,
			     unsigned long clone_flags, u64 now);
extern void sched_bore_init(void);

/* Provided by fair.c; avoids moving reweight_entity() out of CFS. */
extern void reweight_task_bore(struct task_struct *p, int prio);
#endif /* CONFIG_SCHED_BORE */

#endif /* _LINUX_SCHED_BORE_H */
