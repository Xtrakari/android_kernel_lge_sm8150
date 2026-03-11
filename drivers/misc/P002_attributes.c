/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * P002 Kernel Extensions Framework 
 *
 * Copyright (C) 2026 RapliVx
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/fb.h>

#if IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM)
#include <linux/msm_drm_notify.h>
#endif

#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/p002_attributes.h>

/* Task Killer State */
static char p002_bg_blocklist[P002_BLOCKLIST_STRLEN] = "com.shopee.my,com.thecarousell.Carousell";
static char restricted_apps[P002_MAX_BLOCKED][TASK_COMM_LEN];
static u8   restricted_len[P002_MAX_BLOCKED];
static int  restricted_cnt;

/* I/O Switcher State */
#define RESTORE_DELAY_MS 5000
static char p002_sleep_iosched[ELV_NAME_MAX] = "anxiety";
static bool screen_is_off = false;

struct p002_queue_entry {
    struct list_head list;
    struct request_queue *q;
    char prev_e[ELV_NAME_MAX];
    bool is_sleeping;
};

static LIST_HEAD(p002_queues);
static DEFINE_SPINLOCK(p002_io_lock);
static struct delayed_work io_switch_work;
static struct kobject *p002_kobj;


/*
 * Event-Driven Task Killer
 */

static bool p002_is_restricted(const char *comm)
{
    int i;
    for (i = 0; i < restricted_cnt; i++) {
        if (!strncmp(comm, restricted_apps[i], restricted_len[i]))
            return true;
    }
    return false;
}

void p002_background_event(struct task_struct *task, short oom_adj)
{
    if (oom_adj >= 200) {
        if (unlikely(p002_is_restricted(task->comm))) {
            pr_info("P002: INSTANT KILL! '%s' (PID: %d) entered background (OOM: %d)\n", 
                    task->comm, task->pid, oom_adj);
            send_sig(SIGKILL, task, 0);
        }
    }
}
EXPORT_SYMBOL_GPL(p002_background_event);


/*
 * I/O Scheduler Switcher
 */

static void p002_do_io_switch(struct work_struct *work)
{
    struct p002_queue_entry *entry;
    
    spin_lock(&p002_io_lock);
    list_for_each_entry(entry, &p002_queues, list) {
        struct request_queue *q = entry->q;
        
        if (screen_is_off && !entry->is_sleeping) {
            if (q->elevator && q->elevator->type) {
                strlcpy(entry->prev_e, q->elevator->type->elevator_name, ELV_NAME_MAX);
                
                if (elevator_init(q, p002_sleep_iosched) == 0) {
                    pr_info("P002: I/O Switch -> %s (Screen Off)\n", p002_sleep_iosched);
                    entry->is_sleeping = true;
                }
            }
        } else if (!screen_is_off && entry->is_sleeping) {
            if (elevator_init(q, entry->prev_e) == 0) {
                pr_info("P002: I/O Restore -> %s (Screen On)\n", entry->prev_e);
                entry->is_sleeping = false;
            }
        }
    }
    spin_unlock(&p002_io_lock);
}

int p002_init_iosched_switcher(struct request_queue *q)
{
    struct p002_queue_entry *entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) 
        return -ENOMEM;

    entry->q = q;
    spin_lock(&p002_io_lock);
    list_add(&entry->list, &p002_queues);
    spin_unlock(&p002_io_lock);
    
    pr_info("P002: I/O Queue registered to switcher.\n");
    return 0;
}
EXPORT_SYMBOL_GPL(p002_init_iosched_switcher);


/*
 * Universal Display Notifier (DRM & Framebuffer)
 */

#if IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM)

struct msm_drm_notifier_data {
    void *dev;
    void *data;
};

static int p002_display_notifier_callback(struct notifier_block *nb, unsigned long action, void *data)
{
    struct msm_drm_notifier_data *evdata = (struct msm_drm_notifier_data *)data;
    int *blank;

    if (!evdata || !evdata->data)
        return NOTIFY_OK;

    if (action != MSM_DRM_EARLY_EVENT_BLANK && action != MSM_DRM_EVENT_BLANK)
        return NOTIFY_OK;

    blank = (int *)evdata->data;

    if (*blank == MSM_DRM_BLANK_UNBLANK) {
        screen_is_off = false;
    } else if (*blank == MSM_DRM_BLANK_POWERDOWN) {
        screen_is_off = true;
    }

    schedule_delayed_work(&io_switch_work, msecs_to_jiffies(RESTORE_DELAY_MS));
    return NOTIFY_OK;
}

#else
static int p002_display_notifier_callback(struct notifier_block *nb, unsigned long action, void *data)
{
    struct fb_event *evdata = data;
    int *blank = evdata->data;

    if (action != FB_EARLY_EVENT_BLANK && action != FB_EVENT_BLANK)
        return NOTIFY_OK;

    if (*blank == FB_BLANK_UNBLANK) {
        screen_is_off = false;
    } else if (*blank == FB_BLANK_POWERDOWN || *blank == FB_BLANK_NORMAL) {
        screen_is_off = true;
    }

    schedule_delayed_work(&io_switch_work, msecs_to_jiffies(RESTORE_DELAY_MS));
    return NOTIFY_OK;
}
#endif

static struct notifier_block p002_display_nb = {
    .notifier_call = p002_display_notifier_callback,
};


/*
 * Sysfs Interfaces
 */

static void p002_rebuild_blocklist(char *buf)
{
    char *p = buf;
    char *token;

    restricted_cnt = 0;
    while ((token = strsep(&p, ",")) && restricted_cnt < P002_MAX_BLOCKED) {
        if (!*token) continue;
        strlcpy(restricted_apps[restricted_cnt], token, TASK_COMM_LEN);
        restricted_len[restricted_cnt] = strlen(restricted_apps[restricted_cnt]);
        restricted_cnt++;
    }
    pr_info("P002: Blocklist updated. Total active: %d\n", restricted_cnt);
}

static ssize_t bg_blocklist_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%s\n", p002_bg_blocklist);
}

static ssize_t bg_blocklist_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    char tmp[P002_BLOCKLIST_STRLEN];
    strlcpy(tmp, buf, sizeof(tmp));
    strreplace(tmp, '\n', '\0');
    strlcpy(p002_bg_blocklist, tmp, sizeof(p002_bg_blocklist));
    p002_rebuild_blocklist(tmp);
    return count;
}

static ssize_t sleep_iosched_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%s\n", p002_sleep_iosched);
}

static ssize_t sleep_iosched_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    strlcpy(p002_sleep_iosched, buf, ELV_NAME_MAX);
    strreplace(p002_sleep_iosched, '\n', '\0');
    pr_info("P002: Sleep I/O scheduler set to: %s\n", p002_sleep_iosched);
    return count;
}

static struct kobj_attribute bg_blocklist_attr = __ATTR(bg_blocklist, 0664, bg_blocklist_show, bg_blocklist_store);
static struct kobj_attribute sleep_iosched_attr = __ATTR(sleep_iosched, 0664, sleep_iosched_show, sleep_iosched_store);

static struct attribute *p002_attrs[] = {
    &bg_blocklist_attr.attr,
    &sleep_iosched_attr.attr,
    NULL,
};

static const struct attribute_group p002_attr_group = { .attrs = p002_attrs };


/* 
 * Framework Initialization
 */

static int __init p002_attributes_init(void) 
{
    int ret;
    char tmp[P002_BLOCKLIST_STRLEN];

    pr_info("P002: Initializing Framework v2.3 ...\n");

    /* Init Task Killer Blocklist */
    strlcpy(tmp, p002_bg_blocklist, sizeof(tmp));
    p002_rebuild_blocklist(tmp);

    /* Init I/O Switcher & Register Universal Display Notifier */
    INIT_DELAYED_WORK(&io_switch_work, p002_do_io_switch);

#if IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM)
    msm_drm_register_client(&p002_display_nb);
    pr_info("P002: DRM Display Notifier Registered\n");
#else
    fb_register_client(&p002_display_nb);
    pr_info("P002: Legacy FB Display Notifier Registered\n");
#endif

    /* Create Sysfs Directory /sys/kernel/p002/ */
    p002_kobj = kobject_create_and_add("p002", kernel_kobj);
    if (!p002_kobj) 
        return -ENOMEM;

    ret = sysfs_create_group(p002_kobj, &p002_attr_group);
    if (ret) {
        kobject_put(p002_kobj);
        return ret;
    }

    return 0;
}
core_initcall(p002_attributes_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RapliVx");
MODULE_DESCRIPTION("P002 Framework: Universal OOM Killer & I/O Switcher");
MODULE_VERSION("2.3");