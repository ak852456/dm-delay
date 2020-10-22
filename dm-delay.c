#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/timer.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "delay"

struct delay_class {
	struct dm_dev *dev;
	sector_t start;		// new disk block address
	unsigned delay;		// delay in microseconds
	unsigned ops;		
};

struct delay_c {
	struct hrtimer delay_timer;
	struct mutex timer_lock;
	struct workqueue_struct *kdelayd_wq;
	struct work_struct flush_expired_bios;
	struct list_head delayed_bios;
	atomic_t may_delay;

	struct delay_class read;
	struct delay_class write;
	struct delay_class flush;

	int argc;
};

struct dm_delay_info {
	struct delay_c *context;
	struct delay_class *class;
	struct list_head list;
	ktime_t expires;
};

static DEFINE_MUTEX(delayed_bios_lock);

static int ktime_lt(const ktime_t x, const ktime_t y)
{
	return x < y;
}

static int ktime_lteq(const ktime_t x, const ktime_t y)
{
	return x <= y;
}

static ktime_t get_ktime_min(const ktime_t x, const ktime_t y)
{
	return (x < y) ? x : y;
}

static enum hrtimer_restart handle_delayed_timer(struct hrtimer *hrt)
{
	struct delay_c *dc = from_timer(dc, hrt, delay_timer);	// container_of
	queue_work(dc->kdelayd_wq, &dc->flush_expired_bios);	// add work to workqueue
	return HRTIMER_NORESTART;
}

static void queue_timeout(struct delay_c *dc, ktime_t expires)
{
	mutex_lock(&dc->timer_lock);

	if (!hrtimer_active(&dc->delay_timer) || ktime_lt(expires, hrtimer_get_expires(&dc->delay_timer)))
		hrtimer_start(&dc->delay_timer, expires, HRTIMER_MODE_ABS);
	
	mutex_unlock(&dc->timer_lock);
}

static void flush_bios(struct bio *bio)
{
	struct bio *n;

	while (bio) {
		n = bio->bi_next;
		bio->bi_next = NULL;
		generic_make_request(bio);
		bio = n;
	}
}

static struct bio *flush_delayed_bios(struct delay_c *dc, int flush_all)
{
	struct dm_delay_info *delayed, *next;
	ktime_t next_expires = 0, current_time;
	unsigned long start_timer = 0;
	struct bio_list flush_bios = { };

	mutex_lock(&delayed_bios_lock);
	current_time = ktime_get();
	
	list_for_each_entry_safe(delayed, next, &dc->delayed_bios, list)
	{
		if (flush_all || ktime_lteq(delayed->expires, current_time))
		{
			struct bio *bio = dm_bio_from_per_bio_data(delayed, sizeof(struct dm_delay_info));
			bio_list_add(&flush_bios, bio);
			list_del(&delayed->list);
			
			delayed->class->ops--;
			continue;
		}

		if (!start_timer)
		{
			start_timer = 1;
			next_expires = delayed->expires;
		} 
		
		else
			next_expires = get_ktime_min(next_expires, delayed->expires);
	}
	
	mutex_unlock(&delayed_bios_lock);
	if (start_timer)
		queue_timeout(dc, next_expires);

	return bio_list_get(&flush_bios);
}

static void flush_expired_bios_func(struct work_struct *work)
{
	struct delay_c *dc;
	dc = container_of(work, struct delay_c, flush_expired_bios);
	flush_bios(flush_delayed_bios(dc, 0));
}

static void delay_dtr(struct dm_target *ti)
{
	struct delay_c *dc = ti->private;

	if (dc->kdelayd_wq)
		destroy_workqueue(dc->kdelayd_wq);

	if (dc->read.dev)
		dm_put_device(ti, dc->read.dev);
	
	if (dc->write.dev)
		dm_put_device(ti, dc->write.dev);
	
	if (dc->flush.dev)
		dm_put_device(ti, dc->flush.dev);

	mutex_destroy(&dc->timer_lock);
	kfree(dc);
}

static int delay_class_ctr(struct dm_target *ti, struct delay_class *c, char **argv)
{
	int ret;
	unsigned long long tmpll;
	char dummy;

	if (sscanf(argv[1], "%llu%c", &tmpll, &dummy) != 1 || tmpll != (sector_t)tmpll) {
		ti->error = "Invalid device sector";
		return -EINVAL;
	}
	
	c->start = tmpll;
	if (sscanf(argv[2], "%u%c", &c->delay, &dummy) != 1) {
		ti->error = "Invalid delay";
		return -EINVAL;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		return ret;
	}

	return 0;
}

/*
 * Mapping parameters:
 *    <device> <offset> <delay> [<write_device> <write_offset> <write_delay>]
 *
 * With separate write parameters, the first set is only used for reads.
 * Offsets are specified in sectors.
 * Delays are specified in microseconds.
 */
static int delay_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct delay_c *dc;
	int ret;

	if (argc != 3 && argc != 6 && argc != 9) {
		ti->error = "Requires exactly 3, 6 or 9 arguments";
		return -EINVAL;
	}

	dc = kzalloc(sizeof(*dc), GFP_KERNEL);
	if (!dc) {
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}

	ti->private = dc;
	hrtimer_init(&dc->delay_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	dc->delay_timer.function = handle_delayed_timer;
	
	INIT_WORK(&dc->flush_expired_bios, flush_expired_bios_func);
	INIT_LIST_HEAD(&dc->delayed_bios);
	mutex_init(&dc->timer_lock);
	atomic_set(&dc->may_delay, 1);
	dc->argc = argc;

	ret = delay_class_ctr(ti, &dc->read, argv);
	if (ret)
		goto bad;

	if (argc == 3) {
		ret = delay_class_ctr(ti, &dc->write, argv);
		if (ret)
			goto bad;
		ret = delay_class_ctr(ti, &dc->flush, argv);
		if (ret)
			goto bad;
		goto out;
	}

	ret = delay_class_ctr(ti, &dc->write, argv + 3);
	if (ret)
		goto bad;
	if (argc == 6) {
		ret = delay_class_ctr(ti, &dc->flush, argv + 3);
		if (ret)
			goto bad;
		goto out;
	}

	ret = delay_class_ctr(ti, &dc->flush, argv + 6);
	if (ret)
		goto bad;

out:
	dc->kdelayd_wq = alloc_workqueue("kdelayd", WQ_MEM_RECLAIM, 0);
	if (!dc->kdelayd_wq) {
		ret = -EINVAL;
		DMERR("Couldn't start kdelayd");
		goto bad;
	}

	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->per_io_data_size = sizeof(struct dm_delay_info);
	return 0;

bad:
	delay_dtr(ti);
	return ret;
}

static int delay_bio(struct delay_c *dc, struct delay_class *class, struct bio *bio)
{
	struct dm_delay_info *delayed;
	if (!class->delay || !atomic_read(&dc->may_delay))
		return DM_MAPIO_REMAPPED;

	delayed = dm_per_bio_data(bio, sizeof(struct dm_delay_info));
	delayed->context = dc;
	delayed->expires = ktime_add_us(ktime_get(), class->delay);

	mutex_lock(&delayed_bios_lock);
	class->ops++;
	list_add_tail(&delayed->list, &dc->delayed_bios);
	mutex_unlock(&delayed_bios_lock);

	queue_timeout(dc, delayed->expires);
	return DM_MAPIO_SUBMITTED;
}

static int delay_map(struct dm_target *ti, struct bio *bio)
{
	struct delay_c *dc = ti->private;
	struct delay_class *class;
	struct dm_delay_info *delayed = dm_per_bio_data(bio, sizeof(struct dm_delay_info));

	if (bio_data_dir(bio) == WRITE) {
		if (unlikely(bio->bi_opf & REQ_PREFLUSH))
			class = &dc->flush;
		else
			class = &dc->write;
	} else {
		class = &dc->read;
	}
	
	delayed->class = class;
	bio_set_dev(bio, class->dev->bdev);
	if (bio_sectors(bio))
		bio->bi_iter.bi_sector = class->start + dm_target_offset(ti, bio->bi_iter.bi_sector);

	return delay_bio(dc, class, bio);
}

static void delay_presuspend(struct dm_target *ti)
{
	struct delay_c *dc = ti->private;
	atomic_set(&dc->may_delay, 0);
	hrtimer_cancel(&dc->delay_timer);
	flush_bios(flush_delayed_bios(dc, 1));
}

static void delay_resume(struct dm_target *ti)
{
	struct delay_c *dc = ti->private;
	atomic_set(&dc->may_delay, 1);
}

#define DMEMIT_DELAY_CLASS(c) \
	DMEMIT("%s %llu %u", (c)->dev->name, (unsigned long long)(c)->start, (c)->delay)

static void delay_status(struct dm_target *ti, status_type_t type, unsigned status_flags, char *result, unsigned maxlen)
{
	struct delay_c *dc = ti->private;
	int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%u %u %u", dc->read.ops, dc->write.ops, dc->flush.ops);
		break;

	case STATUSTYPE_TABLE:
		DMEMIT_DELAY_CLASS(&dc->read);
		if (dc->argc >= 6) {
			DMEMIT(" ");
			DMEMIT_DELAY_CLASS(&dc->write);
		}
		if (dc->argc >= 9) {
			DMEMIT(" ");
			DMEMIT_DELAY_CLASS(&dc->flush);
		}
		break;
	}
}

static int delay_iterate_devices(struct dm_target *ti, iterate_devices_callout_fn fn, void *data)
{
	struct delay_c *dc = ti->private;
	int ret = 0;

	ret = fn(ti, dc->read.dev, dc->read.start, ti->len, data);
	if (ret)
		goto out;
	ret = fn(ti, dc->write.dev, dc->write.start, ti->len, data);
	if (ret)
		goto out;
	ret = fn(ti, dc->flush.dev, dc->flush.start, ti->len, data);
	if (ret)
		goto out;

out:
	return ret;
}

static struct target_type delay_target = {
	.name	     = "delay",
	.version     = {1, 2, 0},
	.features    = DM_TARGET_PASSES_INTEGRITY,
	.module      = THIS_MODULE,
	.ctr	     = delay_ctr,
	.dtr	     = delay_dtr,
	.map	     = delay_map,
	.presuspend  = delay_presuspend,
	.resume	     = delay_resume,
	.status	     = delay_status,
	.iterate_devices = delay_iterate_devices,
};

static int __init dm_delay_init(void)
{
	int r;
    printk(KERN_INFO "Initialize the dm delay!!!\n");

	r = dm_register_target(&delay_target);
	if (r < 0) {
		DMERR("register failed %d", r);
		goto bad_register;
	}

	return 0;

bad_register:
	return r;
}

static void __exit dm_delay_exit(void)
{
	dm_unregister_target(&delay_target);
}

/* Module hooks */
module_init(dm_delay_init);
module_exit(dm_delay_exit);

MODULE_DESCRIPTION(DM_NAME " delay target");
MODULE_AUTHOR("LEE");
MODULE_LICENSE("GPL");
