// SPDX-License-Identifier: GPL-2.0
/*
 * Burst-Oriented Response Enhancer (BORE) 5.9.6 core logic backported
 * to the 4.14 CFS scheduler.
 *
 * EEVDF-only pieces from upstream 5.9.6 (deadline, vlag, slice,
 * rel_deadline, parity) are intentionally not ported.
 *
 * Copyright (C) 2021-2025 Masahito Suzuki <firelzrd@gmail.com>
 */
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/bore.h>

#include "sched.h"

#ifdef CONFIG_SCHED_BORE

unsigned int __read_mostly sched_bore			= 1;
unsigned int __read_mostly sched_burst_exclude_kthreads	= 1;
unsigned int __read_mostly sched_burst_smoothness_long	= 1;
unsigned int __read_mostly sched_burst_smoothness_short	= 0;
unsigned int __read_mostly sched_burst_fork_atavistic	= 2;
unsigned int __read_mostly sched_burst_parity_threshold	= 2;
unsigned int __read_mostly sched_burst_penalty_offset	= 24;
unsigned int __read_mostly sched_burst_penalty_scale	= 1280;
unsigned int __read_mostly sched_burst_cache_stop_count	= 64;
unsigned int __read_mostly sched_burst_cache_lifetime	= 75000000;

#define MAX_BURST_PENALTY (39U << 2)

/* entity_is_task()/task_of() are local to fair.c in 4.14. */
#ifdef CONFIG_FAIR_GROUP_SCHED
#define bore_entity_is_task(se) (!((se)->my_q))
#else
#define bore_entity_is_task(se) 1
#endif

static inline struct task_struct *bore_task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}

static inline u32 log2plus1_u64_u32f8(u64 v)
{
	u32 integral;
	u8 fractional;

	if (!v)
		return 0;

	integral = fls64(v);
	fractional = (u8)(v << (64 - integral) >> 55);
	return integral << 8 | fractional;
}

static inline u32 calc_burst_penalty(u64 burst_time)
{
	u32 greed, scaled_penalty;
	s32 tolerance, diff, penalty;

	greed = log2plus1_u64_u32f8(burst_time);
	tolerance = (s32)(sched_burst_penalty_offset << 8);
	diff = (s32)greed - tolerance;
	penalty = max_t(s32, 0, diff);
	scaled_penalty = (u32)penalty * sched_burst_penalty_scale >> 16;

	return min_t(u32, MAX_BURST_PENALTY, scaled_penalty);
}

static inline u8 bore_effective_prio(struct task_struct *p)
{
	u8 prio = (u8)(p->static_prio - MAX_RT_PRIO);

	if (likely(sched_bore))
		prio += p->se.burst_score;
	return min_t(u8, 39, prio);
}

void update_burst_score(struct sched_entity *se)
{
	struct task_struct *p;
	u8 prev_prio, burst_score = 0, new_prio;

	if (!bore_entity_is_task(se))
		return;

	p = bore_task_of(se);
	prev_prio = bore_effective_prio(p);
	if (!((p->flags & PF_KTHREAD) && likely(sched_burst_exclude_kthreads)))
		burst_score = (u8)(se->burst_penalty >> 2);

	se->burst_score = burst_score;
	new_prio = bore_effective_prio(p);
	if (new_prio != prev_prio)
		reweight_task_bore(p, new_prio);
}

void update_burst_penalty(struct sched_entity *se)
{
	se->curr_burst_penalty = (u8)calc_burst_penalty(se->burst_time);
	se->burst_penalty = max_t(u8, se->prev_burst_penalty,
				  se->curr_burst_penalty);
	update_burst_score(se);
}

static inline u32 binary_smooth(u32 new, u32 old)
{
	int increment = (int)new - (int)old;

	if (increment >= 0)
		return old + ((u32)increment >> sched_burst_smoothness_long);
	return old - ((u32)-increment >> sched_burst_smoothness_short);
}

static inline void revolve_burst_penalty(struct sched_entity *se)
{
	se->prev_burst_penalty = (u8)binary_smooth(se->curr_burst_penalty,
						  se->prev_burst_penalty);
	se->burst_time = 0;
	se->curr_burst_penalty = 0;
}

void restart_burst(struct sched_entity *se)
{
	revolve_burst_penalty(se);
	se->burst_penalty = se->prev_burst_penalty;
	update_burst_score(se);
}

static inline bool task_is_bore_eligible(struct task_struct *p)
{
	return p && p->sched_class == &fair_sched_class && !p->exit_state;
}

#define for_each_child_task(p, t) \
	list_for_each_entry(t, &(p)->children, sibling)

static inline u32 count_entries_upto2(struct list_head *head)
{
	struct list_head *next = head->next;

	return (next != head) + (next->next != head);
}

static inline void init_task_burst_cache_lock(struct task_struct *p)
{
	spin_lock_init(&p->se.child_burst.lock);
	spin_lock_init(&p->se.group_burst.lock);
}

static inline bool burst_cache_expired(struct sched_burst_cache *bc, u64 now)
{
	return (s64)(bc->timestamp + sched_burst_cache_lifetime - now) < 0;
}

static void update_burst_cache(struct sched_burst_cache *bc,
			       struct task_struct *p, u32 cnt, u32 sum, u64 now)
{
	u8 avg = cnt ? (u8)(sum / cnt) : 0;

	bc->score = max_t(u8, avg, p->se.burst_penalty);
	bc->count = cnt;
	bc->timestamp = now;
}

static void update_child_burst_direct(struct task_struct *p, u64 now)
{
	struct task_struct *child;
	u32 cnt = 0, sum = 0;

	for_each_child_task(p, child) {
		if (!task_is_bore_eligible(child))
			continue;
		cnt++;
		sum += child->se.burst_penalty;
	}

	update_burst_cache(&p->se.child_burst, p, cnt, sum, now);
}

static u8 inherit_burst_direct(struct task_struct *p, u64 now,
			       unsigned long clone_flags)
{
	struct task_struct *parent = p;
	struct sched_burst_cache *bc;
	u8 penalty;

	if ((clone_flags & CLONE_PARENT) && parent->real_parent)
		parent = parent->real_parent;

	bc = &parent->se.child_burst;
	spin_lock(&bc->lock);
	if (burst_cache_expired(bc, now))
		update_child_burst_direct(parent, now);
	penalty = bc->score;
	spin_unlock(&bc->lock);

	return penalty;
}

static void update_child_burst_topological(struct task_struct *p, u64 now,
					   u32 depth, u32 *acnt, u32 *asum)
{
	struct task_struct *child, *dec;
	struct sched_burst_cache *bc;
	u32 cnt = 0, dcnt = 0, sum = 0;

	for_each_child_task(p, child) {
		dec = child;
		while ((dcnt = count_entries_upto2(&dec->children)) == 1)
			dec = list_first_entry(&dec->children,
					       struct task_struct, sibling);

		if (!dcnt || !depth) {
			if (!task_is_bore_eligible(dec))
				continue;
			cnt++;
			sum += dec->se.burst_penalty;
			continue;
		}

		bc = &dec->se.child_burst;
		spin_lock(&bc->lock);
		if (!burst_cache_expired(bc, now)) {
			cnt += bc->count;
			sum += (u32)bc->score * bc->count;
			if (sched_burst_cache_stop_count <= cnt) {
				spin_unlock(&bc->lock);
				break;
			}
			spin_unlock(&bc->lock);
			continue;
		}
		update_child_burst_topological(dec, now, depth - 1, &cnt, &sum);
		spin_unlock(&bc->lock);
	}

	update_burst_cache(&p->se.child_burst, p, cnt, sum, now);
	*acnt += cnt;
	*asum += sum;
}

static u8 inherit_burst_topological(struct task_struct *p, u64 now,
				    unsigned long clone_flags)
{
	struct task_struct *anc = p, *next;
	struct sched_burst_cache *bc;
	u32 cnt = 0, sum = 0;
	u32 base_child_cnt = 0;
	u8 penalty;

	if ((clone_flags & CLONE_PARENT) && anc->real_parent) {
		anc = anc->real_parent;
		base_child_cnt = 1;
	}

	for (next = anc->real_parent;
	     anc != next && next &&
	     count_entries_upto2(&anc->children) <= base_child_cnt;
	     next = anc->real_parent) {
		anc = next;
		base_child_cnt = 1;
	}

	bc = &anc->se.child_burst;
	spin_lock(&bc->lock);
	if (burst_cache_expired(bc, now))
		update_child_burst_topological(anc, now,
				       sched_burst_fork_atavistic - 1,
				       &cnt, &sum);
	penalty = bc->score;
	spin_unlock(&bc->lock);

	return penalty;
}

static void update_tg_burst(struct task_struct *p, u64 now)
{
	struct task_struct *task;
	u32 cnt = 0, sum = 0;

	for_each_thread(p, task) {
		if (!task_is_bore_eligible(task))
			continue;
		cnt++;
		sum += task->se.burst_penalty;
	}

	update_burst_cache(&p->se.group_burst, p, cnt, sum, now);
}

static u8 inherit_burst_tg(struct task_struct *p, u64 now)
{
	struct task_struct *leader = p->group_leader;
	struct sched_burst_cache *bc = &leader->se.group_burst;
	u8 penalty;

	spin_lock(&bc->lock);
	if (burst_cache_expired(bc, now))
		update_tg_burst(leader, now);
	penalty = bc->score;
	spin_unlock(&bc->lock);

	return penalty;
}

void sched_clone_bore(struct task_struct *p, struct task_struct *parent,
		      unsigned long clone_flags, u64 now)
{
	struct sched_entity *se = &p->se;
	u8 penalty = 0;

	init_task_burst_cache_lock(p);

	if (!task_is_bore_eligible(p))
		return;

	if (clone_flags & CLONE_THREAD) {
		rcu_read_lock();
		penalty = inherit_burst_tg(parent, now);
		rcu_read_unlock();
	} else {
		read_lock(&tasklist_lock);
		penalty = likely(sched_burst_fork_atavistic) ?
			inherit_burst_topological(parent, now, clone_flags) :
			inherit_burst_direct(parent, now, clone_flags);
		read_unlock(&tasklist_lock);
	}

	revolve_burst_penalty(se);
	se->burst_penalty = se->prev_burst_penalty =
		max_t(u8, se->prev_burst_penalty, penalty);
	se->child_burst.timestamp = 0;
	se->group_burst.timestamp = 0;
}

void reset_task_bore(struct task_struct *p)
{
	struct sched_burst_cache *bc;

	p->se.burst_time = 0;
	p->se.prev_burst_penalty = 0;
	p->se.curr_burst_penalty = 0;
	p->se.burst_penalty = 0;
	p->se.burst_score = 0;

	bc = &p->se.child_burst;
	bc->score = 0;
	bc->count = 0;
	bc->timestamp = 0;
	bc = &p->se.group_burst;
	bc->score = 0;
	bc->count = 0;
	bc->timestamp = 0;
	init_task_burst_cache_lock(p);
}

void __init sched_bore_init(void)
{
	pr_info("BORE (Burst-Oriented Response Enhancer) CPU Scheduler modification %s by Masahito Suzuki\n",
		SCHED_BORE_VERSION);
	reset_task_bore(&init_task);
}

#endif /* CONFIG_SCHED_BORE */
