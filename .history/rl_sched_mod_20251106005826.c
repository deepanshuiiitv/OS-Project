/*
 * rl_sched_mod.c
 *
 * Experimental RL-based scheduler prototype as a kernel module.
 * - Monitors processes periodically and uses simple Q-learning to adjust nice values.
 *
 * WARNING:
 *  - Experimental. Use only in test environments (VM).
 *  - This module changes task nice values which affects system behavior.
 *
 * Build:
 *   make
 * Load:
 *   sudo insmod rl_sched_mod.ko
 * Unload:
 *   sudo rmmod rl_sched_mod
 *
 * Module parameters:
 *   alpha_permille, gamma_permille, epsilon_permille, interval_ms, action_step
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/random.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Project");
MODULE_DESCRIPTION("Experimental RL scheduler prototype - adjusts nice values using Q-learning (integer math)");
MODULE_VERSION("0.2");

/* module parameters (scaled as permille integers, e.g. 200 = 0.200) */
static int alpha_permille   = 200;  /* learning rate = 0.200 */
static int gamma_permille   = 900;  /* discount factor = 0.900 */
static int epsilon_permille = 200;  /* exploration prob = 0.200 */
static unsigned int interval_ms = 1000; /* sampling interval in ms */
static int action_step = 5;       /* change in nice per action (capped) */

module_param(alpha_permille, int, 0644);
MODULE_PARM_DESC(alpha_permille, "Learning rate × 1000 (e.g. 200 = 0.2)");

module_param(gamma_permille, int, 0644);
MODULE_PARM_DESC(gamma_permille, "Discount factor × 1000 (e.g. 900 = 0.9)");

module_param(epsilon_permille, int, 0644);
MODULE_PARM_DESC(epsilon_permille, "Exploration prob ×1000 (e.g. 200 = 0.2)");

module_param(interval_ms, uint, 0644);
MODULE_PARM_DESC(interval_ms, "Sampling interval in milliseconds");

module_param(action_step, int, 0644);
MODULE_PARM_DESC(action_step, "Nice change step magnitude");

/* RL definitions */
#define NUM_STATES 3   /* Low / Med / High CPU delta */
#define NUM_ACTIONS 3  /* decrease nice (boost), increase nice (penalize), no-op */

enum rl_state {
    RL_STATE_LOW = 0,
    RL_STATE_MED = 1,
    RL_STATE_HIGH = 2,
};

enum rl_action {
    RL_DEC_NICE = 0, /* decrease nice -> increase priority */
    RL_INC_NICE = 1, /* increase nice -> decrease priority */
    RL_NOOP     = 2,
};

/* Per-pid record */
struct pid_entry {
    pid_t pid;
    unsigned long long prev_runtime; /* previous se.sum_exec_runtime snapshot (ns) */
    long qtable[NUM_STATES][NUM_ACTIONS]; /* Q-values scaled as permille */
    enum rl_state prev_state;
    int prev_action;
    struct list_head list;
};

/* Global list and lock */
static LIST_HEAD(pid_table);
static spinlock_t pid_table_lock;

/* RL worker thread */
static struct task_struct *rl_thread;

/* Utility: categorize cpu delta (ns) into state buckets */
static enum rl_state cpu_delta_to_state(unsigned long long delta_ns)
{
    if (delta_ns < 1000000ULL)       /* < 1ms */
        return RL_STATE_LOW;
    if (delta_ns < 50000000ULL)      /* < 50ms */
        return RL_STATE_MED;
    return RL_STATE_HIGH;
}

/* Find or create pid_entry for pid */
static struct pid_entry *get_pid_entry(pid_t pid)
{
    struct pid_entry *e;

    list_for_each_entry(e, &pid_table, list) {
        if (e->pid == pid)
            return e;
    }

    e = kzalloc(sizeof(*e), GFP_KERNEL);
    if (!e)
        return NULL;
    e->pid = pid;
    e->prev_runtime = 0;
    e->prev_state = RL_STATE_LOW;
    e->prev_action = RL_NOOP;
    memset(e->qtable, 0, sizeof(e->qtable));
    list_add(&e->list, &pid_table);
    return e;
}

static void remove_pid_entry(pid_t pid)
{
    struct pid_entry *e, *tmp;
    list_for_each_entry_safe(e, tmp, &pid_table, list) {
        if (e->pid == pid) {
            list_del(&e->list);
            kfree(e);
            return;
        }
    }
}

/* clamp nice between -20 and 19 */
static int clamp_nice(int nice)
{
    if (nice < -20) return -20;
    if (nice > 19) return 19;
    return nice;
}

/* choose action with epsilon-greedy on qtable row */
static int choose_action(struct pid_entry *pe, enum rl_state st)
{
    u32 r = get_random_u32() % 1000; /* 0..999 */
    if (r < epsilon_permille) {
        return get_random_u32() % NUM_ACTIONS;
    } else {
        long best = LONG_MIN;
        int best_a = 0, a;
        for (a = 0; a < NUM_ACTIONS; a++) {
            long val = pe->qtable[st][a];
            if (val > best) {
                best = val;
                best_a = a;
            }
        }
        return best_a;
    }
}

/* Q-learning update (all values in permille scaling) */
static void q_update(struct pid_entry *pe, enum rl_state s, int a,
                     long reward, enum rl_state s_next)
{
    long q = pe->qtable[s][a];
    long best_next = LONG_MIN;
    int i;

    for (i = 0; i < NUM_ACTIONS; i++) {
        if (pe->qtable[s_next][i] > best_next)
            best_next = pe->qtable[s_next][i];
    }
    if (best_next == LONG_MIN)
        best_next = 0;

    /* Q’ = Q + α * (reward + γ*best_next − Q) / 1000 */
    long tmp = reward + (gamma_permille * best_next) / 1000 - q;
    q = q + (alpha_permille * tmp) / 1000;

    pe->qtable[s][a] = q;
}

/* apply action to task: adjust nice by step */
static void apply_action_to_task(struct task_struct *task, int action)
{
    int new_nice, cur_nice;

    cur_nice = task_nice(task);
    switch (action) {
    case RL_DEC_NICE:
        new_nice = cur_nice - action_step;
        break;
    case RL_INC_NICE:
        new_nice = cur_nice + action_step;
        break;
    case RL_NOOP:
    default:
        new_nice = cur_nice;
        break;
    }
    new_nice = clamp_nice(new_nice);
    if (new_nice != cur_nice) {
        pr_info("rl_sched_mod: PID %d (%s) action=%d nice: %d -> %d\n",
        task->pid, task->comm, action, cur_nice, new_nice);
        set_user_nice(task, new_nice);
    }
}

/* main RL worker */
static int rl_worker(void *arg)
{
    while (!kthread_should_stop()) {
        rcu_read_lock();
        {
            struct task_struct *p;
            for_each_process(p) {
                unsigned long long curr_runtime = 0;
                unsigned long long delta = 0;
                enum rl_state st;
                struct pid_entry *pe;

                if (p->exit_state == EXIT_ZOMBIE || p->exit_state == EXIT_DEAD)
                    continue;

                curr_runtime = (unsigned long long)p->se.sum_exec_runtime;

                spin_lock(&pid_table_lock);
                pe = get_pid_entry(p->pid);
                spin_unlock(&pid_table_lock);
                if (!pe)
                    continue;

                if (pe->prev_runtime == 0) {
                    pe->prev_runtime = curr_runtime;
                    continue;
                }

                delta = (curr_runtime >= pe->prev_runtime) ?
                        (curr_runtime - pe->prev_runtime) : 0;
                st = cpu_delta_to_state(delta);

                {
                    int action = choose_action(pe, st);
                    apply_action_to_task(p, action);

                    /* reward = -(delta / 1000) → negative ms used */
                    long reward = -(long)(delta / 1000000ULL);

                    if (pe->prev_action >= 0 && pe->prev_action < NUM_ACTIONS) {
                        q_update(pe, pe->prev_state, pe->prev_action, reward, st);
                    }

                    pe->prev_state = st;
                    pe->prev_action = action;
                }

                pe->prev_runtime = curr_runtime;
            }
        }
        rcu_read_unlock();

        msleep_interruptible(interval_ms);
    }
    return 0;
}

/* helper to cleanup list */
static void free_all_entries(void)
{
    struct pid_entry *e, *tmp;
    spin_lock(&pid_table_lock);
    list_for_each_entry_safe(e, tmp, &pid_table, list) {
        list_del(&e->list);
        kfree(e);
    }
    spin_unlock(&pid_table_lock);
}

/* module init/exit */
static int __init rl_init(void)
{
    pr_info("rl_sched_mod: init (alpha=%d gamma=%d epsilon=%d interval_ms=%u action_step=%d)\n",
            alpha_permille, gamma_permille, epsilon_permille, interval_ms, action_step);
    spin_lock_init(&pid_table_lock);

    rl_thread = kthread_run(rl_worker, NULL, "rl_sched_thread");
    if (IS_ERR(rl_thread)) {
        pr_err("rl_sched_mod: failed to create worker thread\n");
        rl_thread = NULL;
        return PTR_ERR(rl_thread);
    }
    return 0;
}

static void __exit rl_exit(void)
{
    pr_info("rl_sched_mod: exit\n");
    if (rl_thread)
        kthread_stop(rl_thread);

    free_all_entries();
    pr_info("rl_sched_mod: cleaned up\n");
}

module_init(rl_init);
module_exit(rl_exit);

