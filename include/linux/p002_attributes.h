/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 RapliVx
 * P002 Kernel Extensions - Event-Driven OOM Killer & I/O Switcher
 */

#ifndef _LINUX_P002_ATTRIBUTES_H
#define _LINUX_P002_ATTRIBUTES_H

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/blkdev.h>

#define P002_BLOCKLIST_STRLEN 256
#define P002_MAX_BLOCKED 16

#ifdef CONFIG_P002_ATTRIBUTES

void p002_background_event(struct task_struct *task, short oom_adj);

int p002_init_iosched_switcher(struct request_queue *q);

#else

static inline void p002_background_event(struct task_struct *task, short oom_adj) {}
static inline int p002_init_iosched_switcher(struct request_queue *q) { return 0; }

#endif /* CONFIG_P002_ATTRIBUTES */

#endif /* _LINUX_P002_ATTRIBUTES_H */