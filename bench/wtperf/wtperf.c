/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "wtperf.h"

/* Default values. */
static const CONFIG default_cfg = {
	"WT_TEST",			/* home */
	NULL,				/* uri */
	NULL,				/* conn */
	NULL,				/* logf */
	NULL, NULL, NULL,		/* threads */

#define	OPT_DEFINE_DEFAULT
#include "wtperf_opt.i"
#undef OPT_DEFINE_DEFAULT
};

static const char * const small_config_str =
    "conn_config=\"cache_size=500MB\","
    "table_config=\"lsm_chunk_size=5MB\","
    "icount=500000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=20,"
    "populate_threads=1,"
    "read_threads=8,";

static const char * const med_config_str =
    "conn_config=\"cache_size=1GB\","
    "table_config=\"lsm_chunk_size=20MB\","
    "icount=50000000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=100,"
    "populate_threads=1,"
    "read_threads=16,";

static const char * const large_config_str =
    "conn_config=\"cache_size=2GB\","
    "table_config=\"lsm_chunk_size=50MB\","
    "icount=500000000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=600,"
    "populate_threads=1,"
    "read_threads=16,";

static const char * const debug_cconfig = "verbose=[lsm]";
static const char * const debug_tconfig = "";

static uint8_t g_run_mix_ops[100];	/* run-mix operation schedule */

static uint64_t g_ckpt_ops;		/* checkpoint operations */
static uint64_t g_insert_ops;		/* insert operations */
static uint64_t g_read_ops;		/* read operations */
static uint64_t g_update_ops;		/* update operations */

static uint64_t g_insert_key;		/* insert key */

static volatile int g_ckpt;		/* checkpoint in progress */
static volatile int g_error;		/* thread error */
static volatile int g_stop;		/* notify threads to stop */

/*
 * Atomic update where needed.
 */
#if defined(_lint)
#define	ATOMIC_ADD(v, val)	((v) += (val), (v))
#else
#define	ATOMIC_ADD(v, val)	__sync_add_and_fetch(&(v), val)
#endif

static void	*checkpoint_worker(void *);
static int	 execute_populate(CONFIG *);
static int	 execute_workload(CONFIG *);
static int	 find_table_count(CONFIG *);
static void	*insert_thread(void *);
static void	*monitor(void *);
static void	*populate_thread(void *);
static void	*read_thread(void *);
static int	 start_threads(
		    CONFIG *, CONFIG_THREAD *, u_int, void *(*)(void *));
static int	 stop_threads(CONFIG *, u_int, CONFIG_THREAD *);
static void	*update_thread(void *);
static void	 worker(CONFIG_THREAD *);
static uint64_t	 wtperf_rand(CONFIG *);
static uint64_t	 wtperf_value_range(CONFIG *);

/* We use a couple of WiredTiger library routines to simplify portability. */
extern int	__wt_epoch(void *, struct timespec *);
extern uint32_t	__wt_random(void);

/* Retrieve an ID for the next insert operation. */
static inline uint64_t
get_next_incr(void)
{
	return (ATOMIC_ADD(g_insert_key, 1));
}

/*
 * track_aggregated_update --
 *	Update an operation's tracking structure with new latency information.
 */
static inline void
track_aggregated_update(TRACK *trk, uint64_t nsecs, uint32_t aggregated)
{
	uint64_t v;

	if (trk->aggregated == 0)
		return;

					/* average nanoseconds per call */
	v = (uint64_t)nsecs / aggregated;

	trk->latency += nsecs;		/* track total latency */

	if (v > trk->max_latency)	/* track max/min latency */
		trk->max_latency = (uint32_t)v;
	if (v < trk->min_latency)
		trk->min_latency = (uint32_t)v;

	/*
	 * Update a latency bucket.
	 * First buckets: usecs from 100us to 1000us at 100us each.
	 */
	if (v < us_to_ns(1000))
		trk->us[ns_to_us(v)] += trk->aggregated;

	/*
	 * Second buckets: millseconds from 1ms to 1000ms, at 1ms each.
	 */
	else if (v < ms_to_ns(1000))
		trk->ms[ns_to_ms(v)] += trk->aggregated;

	/*
	 * Third buckets are seconds from 1s to 100s, at 1s each.
	 */
	else if (v < sec_to_ns(100))
		trk->sec[ns_to_sec(v)] += trk->aggregated;

	/* >100 seconds, accumulate in the biggest bucket. */
	else
		trk->sec[ELEMENTS(trk->sec) - 1] += trk->aggregated;

	trk->aggregated = 0;
}

static void
worker(CONFIG_THREAD *thread)
{
	struct timespec *last, _last, *t, _t, *tmp;
	CONFIG *cfg;
	TRACK *trk;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t next_val, nsecs;
	uint32_t aggregated;
	int ret;
	uint8_t last_op, *op, *op_end;
	char *value_buf, *key_buf, *value;

	cfg = thread->cfg;
	conn = cfg->conn;
	session = NULL;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0, "worker: WT_CONNECTION.open_session");
		goto err;
	}
	if ((ret = session->open_cursor(
	    session, cfg->uri, NULL, NULL, &cursor)) != 0) {
		lprintf(cfg,
		    ret, 0, "worker: WT_SESSION.open_cursor: %s", cfg->uri);
		goto err;
	}

	key_buf = thread->key_buf;
	value_buf = thread->value_buf;

	op = thread->schedule;
	op_end = thread->schedule + sizeof(thread->schedule);

	t = &_t;
	last = &_last;
	assert(__wt_epoch(NULL, last) == 0);

	aggregated = 0;
	while (!g_stop) {
		switch (*op) {
		case WORKER_INSERT:
		case WORKER_INSERT_RMW:
			if (cfg->random_range)
				next_val = wtperf_rand(cfg);
			else
				next_val = cfg->icount + get_next_incr();
			break;
		case WORKER_READ:
		case WORKER_UPDATE:
			next_val = wtperf_rand(cfg);

			/*
			 * If the workload is started without a populate phase
			 * we rely on at least one insert to get a valid item
			 * id.
			 */
			if (wtperf_value_range(cfg) < next_val)
				continue;
			break;
		default:
			goto err;		/* can't happen */
		}

		sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, next_val);
		cursor->set_key(cursor, key_buf);

		switch (*op) {
		case WORKER_READ:
			/*
			 * Reads can fail with WT_NOTFOUND: we may be searching
			 * in a random range, or an insert thread might have
			 * updated the last record in the table but not yet
			 * finished the actual insert.  Count failed search in
			 * a random range as a "read".
			 */
			ret = cursor->search(cursor);
			if (ret == 0 || ret == WT_NOTFOUND) {
				trk = &thread->read;
				break;
			}
			goto op_err;
		case WORKER_INSERT_RMW:
			if ((ret = cursor->search(cursor)) != WT_NOTFOUND)
				goto op_err;

			/* The error return reset the cursor's key. */
			cursor->set_key(cursor, key_buf);

			/* FALLTHROUGH */
		case WORKER_INSERT:
			cursor->set_value(cursor, value_buf);
			if ((ret = cursor->insert(cursor)) == 0) {
				trk = &thread->insert;
				break;
			}
			goto op_err;
		case WORKER_UPDATE:
			if ((ret = cursor->search(cursor)) == 0) {
				assert(cursor->get_value(cursor, &value) == 0);
				memcpy(value_buf, value, cfg->data_sz);
				if (value_buf[0] == 'a')
					value_buf[0] = 'b';
				else
					value_buf[0] = 'a';
				cursor->set_value(cursor, value_buf);
				if ((ret = cursor->update(cursor)) == 0) {
					trk = &thread->update;
					break;
				}
				goto op_err;
			}

			/*
			 * Reads can fail with WT_NOTFOUND: we may be searching
			 * in a random range, or an insert thread might have
			 * updated the last record in the table but not yet
			 * finished the actual insert.  Count failed search in
			 * a random range as a "read".
			 */
			if (ret == WT_NOTFOUND) {
				trk = &thread->read;
				break;
			}

op_err:			lprintf(cfg, ret, 0,
			    "%s failed for: %s, range: %"PRIu64,
			    op_name(op), key_buf, wtperf_value_range(cfg));
			goto err;
		default:
			goto err;		/* can't happen */
		}

		++trk->ops;		/* increment operation counts */
		++trk->aggregated;
		++aggregated;

		last_op = *op;
		if (++op == op_end)	/* schedule the next operation */
			op = thread->schedule;

		/*
		 * Stop aggregation if the operation is going to change or we
		 * reach the configurable limit.
		 */
		if (aggregated < cfg->latency_aggregate && last_op == *op)
			continue;

					/* calculate how long the calls took */
		assert(__wt_epoch(NULL, t) == 0);
		nsecs = (uint64_t)(t->tv_nsec - last->tv_nsec);
		nsecs += sec_to_ns((uint64_t)(t->tv_sec - last->tv_sec));

					/* update call latencies */
		track_aggregated_update(&thread->insert, nsecs, aggregated);
		track_aggregated_update(&thread->read, nsecs, aggregated);
		track_aggregated_update(&thread->update, nsecs, aggregated);
		aggregated = 0;

		tmp = last;		/* swap timers */
		last = t;
		t = tmp;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		g_error = g_stop = 1;
	}

	if (session != NULL)
		assert(session->close(session, NULL) == 0);
}

/*
 * run_mix_schedule_op --
 *	Replace read operations with another operation, in the configured
 * percentage.
 */
static void
run_mix_schedule_op(int op, uint32_t op_cnt)
{
	u_int i, jump;
	uint8_t *p, *end;

	/* Jump around the array to roughly spread out the operations. */
	jump = 100 / op_cnt;

	/*
	 * Find a read operation and replace it with another operation.  This
	 * is roughly n-squared, but it's an N of 100, leave it.
	 */
	p = g_run_mix_ops;
	end = g_run_mix_ops + sizeof(g_run_mix_ops);
	for (i = 0; i < op_cnt; ++i) {
		for (; *p != WORKER_READ; ++p)
			if (p == end)
				p = g_run_mix_ops;
		*p = (uint8_t)op;

		if (end - jump < p)
			p = g_run_mix_ops;
		else
			p += jump;
	}
}

/*
 * run_mix_schedule --
 *	Schedule the mixed-run operations.
 */
static void
run_mix_schedule(CONFIG *cfg)
{
	/* Default to read, then fill in other operations. */
	memset(g_run_mix_ops, WORKER_READ, sizeof(g_run_mix_ops));
	if (cfg->run_mix_inserts)
		run_mix_schedule_op(
		    cfg->insert_rmw ? WORKER_INSERT_RMW : WORKER_INSERT,
		    cfg->run_mix_inserts);
	if (cfg->run_mix_updates)
		run_mix_schedule_op(WORKER_UPDATE, cfg->run_mix_updates);
}

/*
 * op_setup --
 *	Set up the thread's operation list.
 */
static void
op_setup(CONFIG *cfg, int op, CONFIG_THREAD *thread)
{

	/*
	 * If we're not running a job mix, it's easy, all of the operations
	 * are the same.
	 */
	if (cfg->run_mix_inserts == 0 && cfg->run_mix_updates == 0)
		memset(thread->schedule, op, sizeof(thread->schedule));
	else
		memcpy(
		    thread->schedule, g_run_mix_ops, sizeof(thread->schedule));
}

static void *
read_thread(void *arg)
{
	CONFIG_THREAD *thread;

	thread = (CONFIG_THREAD *)arg;

	op_setup(thread->cfg, WORKER_READ, thread);
	worker(thread);
	return (NULL);
}

static void *
insert_thread(void *arg)
{
	CONFIG_THREAD *thread;

	thread = (CONFIG_THREAD *)arg;

	op_setup(thread->cfg,
	    thread->cfg->insert_rmw ? WORKER_INSERT_RMW : WORKER_INSERT,
	    thread);
	worker(thread);
	return (NULL);
}

static void *
update_thread(void *arg)
{
	CONFIG_THREAD *thread;

	thread = (CONFIG_THREAD *)arg;

	op_setup(thread->cfg, WORKER_UPDATE, thread);
	worker(thread);
	return (NULL);
}

static void *
populate_thread(void *arg)
{
	CONFIG *cfg;
	CONFIG_THREAD *thread;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint32_t opcount;
	uint64_t op;
	int intxn, ret;
	char *value_buf, *key_buf;

	thread = (CONFIG_THREAD *)arg;
	cfg = thread->cfg;
	conn = cfg->conn;
	session = NULL;
	ret = 0;

	key_buf = thread->key_buf;
	value_buf = thread->value_buf;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0, "populate: WT_CONNECTION.open_session");
		goto err;
	}
	/* Do a bulk load if populate is single-threaded. */
	if ((ret = session->open_cursor(session, cfg->uri, NULL,
	    cfg->populate_threads == 1 ? "bulk" : NULL, &cursor)) != 0) {
		lprintf(cfg,
		    ret, 0, "populate: WT_SESSION.open_cursor: %s", cfg->uri);
		goto err;
	}

	/* Populate the database. */
	if (cfg->populate_ops_per_txn == 0)
		for (;;) {
			op = get_next_incr();
			if (op > cfg->icount)
				break;

			sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, op);
			cursor->set_key(cursor, key_buf);
			cursor->set_value(cursor, value_buf);
			if ((ret = cursor->insert(cursor)) != 0) {
				lprintf(cfg, ret, 0, "Failed inserting");
				goto err;
			}
			++thread->insert.ops;
		}
	else {
		for (intxn = 0, opcount = 0;;) {
			op = get_next_incr();
			if (op > cfg->icount)
				break;

			if (!intxn) {
				assert(session->begin_transaction(
				    session, cfg->transaction_config) == 0);
				intxn = 1;
			}
			sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, op);
			cursor->set_key(cursor, key_buf);
			cursor->set_value(cursor, value_buf);
			if ((ret = cursor->insert(cursor)) != 0) {
				lprintf(cfg, ret, 0, "Failed inserting");
				goto err;
			}
			++thread->insert.ops;

			if (++opcount < cfg->populate_ops_per_txn)
				continue;
			opcount = 0;

			if ((ret =
			    session->commit_transaction(session, NULL)) != 0)
				lprintf(cfg, ret, 0,
				    "Fail committing, transaction was aborted");
			intxn = 0;
		}
		if (intxn &&
		    (ret = session->commit_transaction(session, NULL)) != 0)
			lprintf(cfg, ret, 0,
			    "Fail committing, transaction was aborted");
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		g_error = g_stop = 1;
	}

	if (session != NULL)
		assert(session->close(session, NULL) == 0);
	return (NULL);
}

static void *
monitor(void *arg)
{
	struct timespec t;
	struct tm *tm, _tm;
	CONFIG *cfg;
	FILE *fp;
	uint64_t reads, inserts, updates;
	uint64_t last_reads, last_inserts, last_updates;
	uint32_t read_avg, read_min, read_max;
	uint32_t insert_avg, insert_min, insert_max;
	uint32_t update_avg, update_min, update_max;
	size_t len;
	u_int i;
	char buf[64], *path;

	cfg = (CONFIG *)arg;
	fp = NULL;
	path = NULL;

	/* Open the logging file. */
	len = strlen(cfg->home) + 100;
	if ((path = malloc(len)) == NULL) {
		(void)enomem(cfg);
		goto err;
	}
	snprintf(path, len, "%s/monitor", cfg->home);
	if ((fp = fopen(path, "w")) == NULL) {
		lprintf(cfg, errno, 0, "%s", path);
		goto err;
	}
#ifdef __WRITE_A_HEADER
	fprintf(fp,
	    "#time,"
	    "read operations,insert operations,update operations,"
	    "checkpoints,"
	    "read average latency(NS),read minimum latency(NS),"
	    "read maximum latency(NS),"
	    "insert average latency(NS),insert min latency(NS),"
	    "insert maximum latency(NS),"
	    "update average latency(NS),update min latency(NS),"
	    "update maximum latency(NS)"
	    "\n");
#endif
	last_reads = last_inserts = last_updates = 0;
	while (!g_stop) {
		for (i = 0; i < cfg->sample_interval; i++) {
			sleep(1);
			if (g_stop)
				break;
		}
		/* If the workers are done, don't bother with a final call. */
		if (g_stop)
			break;

		assert(__wt_epoch(NULL, &t) == 0);
		tm = localtime_r(&t.tv_sec, &_tm);
		(void)strftime(buf, sizeof(buf), "%b %d %H:%M:%S", tm);

		reads = sum_read_ops(cfg);
		inserts = sum_insert_ops(cfg);
		updates = sum_update_ops(cfg);
		latency_read(cfg, &read_avg, &read_min, &read_max);
		latency_insert(cfg, &insert_avg, &insert_min, &insert_max);
		latency_update(cfg, &update_avg, &update_min, &update_max);

		(void)fprintf(fp,
		    "%s"
		    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
		    ",%c"
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    "\n",
		    buf,
		    (reads - last_reads) / cfg->sample_interval,
		    (inserts - last_inserts) / cfg->sample_interval,
		    (updates - last_updates) / cfg->sample_interval,
		    g_ckpt ? 'Y' : 'N',
		    read_avg, read_min, read_max,
		    insert_avg, insert_min, insert_max,
		    update_avg, update_min, update_max);

		last_reads = reads;
		last_inserts = inserts;
		last_updates = updates;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		g_error = g_stop = 1;
	}

	if (fp != NULL)
		(void)fclose(fp);
	free(path);

	return (NULL);
}

static void *
checkpoint_worker(void *arg)
{
	CONFIG *cfg;
	CONFIG_THREAD *thread;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	struct timeval e, s;
	uint64_t ms;
	uint32_t i;
	int ret;

	thread = (CONFIG_THREAD *)arg;
	cfg = thread->cfg;
	conn = cfg->conn;
	session = NULL;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed in checkpoint thread.");
		goto err;
	}

	while (!g_stop) {
		/* Break the sleep up, so we notice interrupts faster. */
		for (i = 0; i < cfg->checkpoint_interval; i++) {
			sleep(1);
			if (g_stop)
				break;
		}
		/* If the workers are done, don't bother with a final call. */
		if (g_stop)
			break;

		assert(gettimeofday(&s, NULL) == 0);
		g_ckpt = 1;
		if ((ret = session->checkpoint(session, NULL)) != 0) {
			lprintf(cfg, ret, 0, "Checkpoint failed.");
			goto err;
		}
		g_ckpt = 0;
		++thread->ckpt.ops;

		assert(gettimeofday(&e, NULL) == 0);
		ms = (e.tv_sec * 1000) + (e.tv_usec / 1000.0);
		ms -= (s.tv_sec * 1000) + (s.tv_usec / 1000.0);
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		g_error = g_stop = 1;
	}

	if (session != NULL)
		assert(session->close(session, NULL) == 0);
	return (NULL);
}

static int
execute_populate(CONFIG *cfg)
{
	struct timeval start, stop;
	double secs;
	uint64_t last_ops;
	uint32_t interval;
	u_int sleepsec;
	int elapsed, ret;

	lprintf(cfg, 0, 1,
	    "Starting %" PRIu32 " populate thread(s)", cfg->populate_threads);

	if ((cfg->popthreads =
	    calloc(cfg->populate_threads, sizeof(CONFIG_THREAD))) == NULL)
		return (enomem(cfg));
	if ((ret = start_threads(cfg,
	    cfg->popthreads, cfg->populate_threads, populate_thread)) != 0)
		return (ret);

	g_insert_key = 0;

	assert(gettimeofday(&start, NULL) == 0);
	for (elapsed = 0, interval = 0, last_ops = 0;
	    g_insert_key < cfg->icount && g_error == 0;) {
		/*
		 * Sleep for 100th of a second, report_interval is in second
		 * granularity, each 100th increment of elapsed is a single
		 * increment of interval.
		 */
		(void)usleep(10000);
		if (cfg->report_interval == 0 || ++elapsed < 100)
			continue;
		elapsed = 0;
		if (++interval < cfg->report_interval)
			continue;
		interval = 0;
		g_insert_ops = sum_pop_ops(cfg);
		lprintf(cfg, 0, 1,
		    "%" PRIu64 " populate inserts in %" PRIu32 " secs",
		    g_insert_ops - last_ops, cfg->report_interval);
		last_ops = g_insert_ops;
	}
	assert(gettimeofday(&stop, NULL) == 0);

	if ((ret =
	    stop_threads(cfg, cfg->populate_threads, cfg->popthreads)) != 0)
		return (ret);

	/* Report if any worker threads didn't finish. */
	if (g_error != 0) {
		lprintf(cfg, WT_ERROR, 0,
		    "Populate thread(s) exited without finishing.");
		return (WT_ERROR);
	}

	lprintf(cfg, 0, 1, "Finished load of %" PRIu32 " items", cfg->icount);
	secs = stop.tv_sec + stop.tv_usec / 1000000.0;
	secs -= start.tv_sec + start.tv_usec / 1000000.0;
	if (secs == 0)
		++secs;
	lprintf(cfg, 0, 1,
	    "Load time: %.2f\n" "load ops/sec: %.2f", secs, cfg->icount / secs);

	/*
	 * If configured, sleep for awhile to allow LSM merging to complete in
	 * the background.  If user specifies -1, then sleep for as long as it
	 * took to load.
	 */
	if (cfg->merge_sleep) {
		if (cfg->merge_sleep < 0)
			sleepsec = (u_int)(stop.tv_sec - start.tv_sec);
		else
			sleepsec = (u_int)cfg->merge_sleep;
		lprintf(cfg, 0, 1, "Sleep %u seconds for merging", sleepsec);
		(void)sleep(sleepsec);
	}

	/*
	 * Reopen the connection.  We do this so that the workload phase always
	 * starts with the on-disk files, and so that read-only workloads can
	 * be identified.  This is particularly important for LSM, where the
	 * merge algorithm is more aggressive for read-only trees.
	 */
	if ((ret = cfg->conn->close(cfg->conn, NULL)) != 0) {
		lprintf(cfg, ret, 0, "Closing the connection failed");
		return (ret);
	}
	if ((ret = wiredtiger_open(
	    cfg->home, NULL, cfg->conn_config, &cfg->conn)) != 0) {
		lprintf(cfg, ret, 0, "Re-opening the connection failed");
		return (ret);
	}

	return (0);
}

static int
execute_workload(CONFIG *cfg)
{
	uint64_t last_ckpts, last_inserts, last_reads, last_updates;
	uint32_t interval, run_ops, run_time;
	int ret, tret;

	g_insert_key = 0;
	g_insert_ops = g_read_ops = g_update_ops = 0;

	last_ckpts = last_inserts = last_reads = last_updates = 0;
	ret = 0;

	if (cfg->run_mix_inserts != 0 || cfg->run_mix_updates != 0)
		lprintf(cfg, 0, 1,
		    "Starting %" PRIu32 " worker threads",
		    cfg->read_threads +
		    cfg->insert_threads + cfg->update_threads);
	else
		lprintf(cfg, 0, 1,
		    "Starting worker threads: read %" PRIu32
		    ", insert %" PRIu32 ", update %" PRIu32,
		    cfg->read_threads,
		    cfg->insert_threads, cfg->update_threads);

	/* Schedule run-mix operations, as necessary. */
	if (cfg->run_mix_inserts != 0 || cfg->run_mix_updates != 0)
		run_mix_schedule(cfg);
	
	/* Start the worker threads. */
	if ((cfg->workers = calloc(
	    cfg->read_threads + cfg->insert_threads + cfg->update_threads,
	    sizeof(CONFIG_THREAD))) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	if ((ret = start_threads(cfg,
	    &cfg->workers[0], cfg->read_threads, read_thread)) != 0)
		goto err;
	if ((ret = start_threads(cfg,
	    &cfg->workers[cfg->read_threads],
	    cfg->insert_threads, insert_thread)) != 0)
		goto err;
	if ((ret = start_threads(cfg,
	    &cfg->workers[cfg->read_threads + cfg->insert_threads],
	    cfg->update_threads, update_thread)) != 0)
		goto err;

	for (interval = cfg->report_interval,
	    run_time = cfg->run_time, run_ops = cfg->run_ops; g_error == 0;) {
		/*
		 * Sleep for one second at a time.
		 * If we are tracking run time, check to see if we're done, and
		 * if we're only tracking run time, go back to sleep.
		 */
		sleep(1);
		if (run_time != 0) {
			if (--run_time == 0)
				break;
			if (!interval && !run_ops)
				continue;
		}

		/* Sum the operations we've done. */
		g_ckpt_ops = sum_ckpt_ops(cfg);
		g_insert_ops = sum_insert_ops(cfg);
		g_read_ops = sum_read_ops(cfg);
		g_update_ops = sum_update_ops(cfg);

		/* If we're checking total operations, see if we're done. */
		if (run_ops != 0 &&
		    run_ops <= g_insert_ops + g_read_ops + g_update_ops)
			break;

		/* If writing out throughput information, see if it's time. */
		if (interval == 0 || --interval > 0)
			continue;
		interval = cfg->report_interval;

		lprintf(cfg, 0, 1,
		    "%" PRIu64 " reads, %" PRIu64 " inserts, %" PRIu64
		    " updates, %" PRIu64 " checkpoints in %" PRIu32 " secs",
		    g_read_ops - last_reads,
		    g_insert_ops - last_inserts,
		    g_update_ops - last_updates,
		    g_ckpt_ops - last_ckpts,
		    cfg->report_interval);
		last_reads = g_read_ops;
		last_inserts = g_insert_ops;
		last_updates = g_update_ops;
		last_ckpts = g_ckpt_ops;
	}

	/* Notify the worker threads they are done. */
err:	g_stop = 1;

	if ((tret = stop_threads(cfg,
	    cfg->read_threads + cfg->insert_threads + cfg->update_threads,
	    cfg->workers)) != 0 && ret == 0)
		ret = tret;

	/* Report if any worker threads didn't finish. */
	if (g_error != 0) {
		lprintf(cfg, WT_ERROR, 0,
		    "Worker thread(s) exited without finishing.");
		if (ret == 0)
			ret = WT_ERROR;
	}
	return (ret);
}

/*
 * Ensure that icount matches the number of records in the 
 * existing table.
 */
static int
find_table_count(CONFIG *cfg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	char *key;
	int ret;

	conn = cfg->conn;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed finding existing table count");
		goto err;
	}
	if ((ret = session->open_cursor(session, cfg->uri,
	    NULL, NULL, &cursor)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_cursor failed finding existing table count");
		goto err;
	}
	if ((ret = cursor->prev(cursor)) != 0) {
		lprintf(cfg, ret, 0,
		    "cursor prev failed finding existing table count");
		goto err;
	}
	assert(cursor->get_key(cursor, &key) == 0);
	cfg->icount = (uint32_t)atoi(key);

err:	assert(session->close(session, NULL) == 0);
	return (ret);
}

int
main(int argc, char *argv[])
{
	CONFIG *cfg, _cfg;
	WT_SESSION *session;
	pthread_t monitor_thread;
	size_t len;
	uint64_t req_len, total_ops;
	int ch, monitor_created, ret, tret;
	const char *opts = "C:O:T:h:o:SML";
	const char *wtperftmp_subdir = "wtperftmp";
	const char *user_cconfig, *user_tconfig;
	char *cmd, *cc_buf, *tc_buf, *tmphome;

	session = NULL;
	monitor_created = ret = 0;
	user_cconfig = user_tconfig = NULL;
	cmd = cc_buf = tc_buf = tmphome = NULL;

	/* Setup the default configuration values. */
	cfg = &_cfg;
	memset(cfg, 0, sizeof(*cfg));
	if (config_assign(cfg, &default_cfg))
		goto err;

	/* Do a basic validation of options, and home is needed before open. */
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'h':
			cfg->home = optarg;
			break;
		case '?':
			fprintf(stderr, "Invalid option\n");
			usage();
			goto einval;
		}

	/*
	 * Create a temporary directory underneath the test directory in which
	 * we do an initial WiredTiger open, because we need a connection and
	 * session in order to use the extension configuration parser.  We will
	 * open the real WiredTiger database after parsing the options.
	 */
	len = strlen(cfg->home) + strlen(wtperftmp_subdir) + 2;
	if ((tmphome = malloc(len)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	snprintf(tmphome, len, "%s/%s", cfg->home, wtperftmp_subdir);
	len = len * 2 + 100;
	if ((cmd = malloc(len)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	snprintf(cmd, len, "rm -rf %s && mkdir %s", tmphome, tmphome);
	if (system(cmd) != 0) {
		fprintf(stderr, "%s: failed\n", cmd);
		goto einval;
	}
	if ((ret = wiredtiger_open(
	    tmphome, NULL, "create", &cfg->conn)) != 0) {
		lprintf(cfg, ret, 0, "wiredtiger_open: %s", tmphome);
		goto err;
	}
	if ((ret = cfg->conn->open_session(
	    cfg->conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0, "Error creating session");
		goto err;
	}

	/*
	 * Then parse different config structures - other options override
	 * fields within the structure.
	 */
	optind = 1;
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'S':
			if (config_opt_line(
			    cfg, session, small_config_str) != 0)
				goto einval;
			break;
		case 'M':
			if (config_opt_line(cfg, session, med_config_str) != 0)
				goto einval;
			break;
		case 'L':
			if (config_opt_line(
			    cfg, session, large_config_str) != 0)
				goto einval;
			break;
		case 'O':
			if (config_opt_file(cfg, session, optarg) != 0)
				goto einval;
			break;
		default:
			/* Validation done previously. */
			break;
		}

	/* Parse other options */
	optind = 1;
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'o':
			/* Allow -o key=value */
			if (config_opt_line(cfg, session, optarg) != 0)
				goto einval;
			break;
		case 'C':
			user_cconfig = optarg;
			break;
		case 'T':
			user_tconfig = optarg;
			break;
		}

	/* Build the URI from the table name. */
	req_len = strlen("table:") + strlen(cfg->table_name) + 1;
	if ((cfg->uri = calloc(req_len, 1)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	snprintf(cfg->uri, req_len, "table:%s", cfg->table_name);
	
	if ((ret = setup_log_file(cfg)) != 0)
		goto err;

	/* Make stdout line buffered, so verbose output appears quickly. */
	(void)setvbuf(stdout, NULL, _IOLBF, 0);

	/* Concatenate non-default configuration strings. */
	if (cfg->verbose > 1 || user_cconfig != NULL) {
		req_len = strlen(cfg->conn_config) + strlen(debug_cconfig) + 3;
		if (user_cconfig != NULL)
			req_len += strlen(user_cconfig);
		if ((cc_buf = calloc(req_len, 1)) == NULL) {
			ret = enomem(cfg);
			goto err;
		}
		snprintf(cc_buf, req_len, "%s%s%s%s%s",
		    cfg->conn_config,
		    cfg->verbose > 1 ? "," : "",
		    cfg->verbose > 1 ? debug_cconfig : "",
		    user_cconfig ? "," : "", user_cconfig ? user_cconfig : "");
		if ((ret = config_opt_str(
		    cfg, session, "conn_config", cc_buf)) != 0)
			goto err;
	}
	if (cfg->verbose > 1 || user_tconfig != NULL) {
		req_len = strlen(cfg->table_config) + strlen(debug_tconfig) + 3;
		if (user_tconfig != NULL)
			req_len += strlen(user_tconfig);
		if ((tc_buf = calloc(req_len, 1)) == NULL) {
			ret = enomem(cfg);
			goto err;
		}
		snprintf(tc_buf, req_len, "%s%s%s%s%s",
		    cfg->table_config,
		    cfg->verbose > 1 ? "," : "",
		    cfg->verbose > 1 ? debug_tconfig : "",
		    user_tconfig ? "," : "", user_tconfig ? user_tconfig : "");
		if ((ret = config_opt_str(
		    cfg, session, "table_config", tc_buf)) != 0)
			goto err;
	}

	ret = session->close(session, NULL);
	session = NULL;
	if (ret != 0) {
		lprintf(cfg, ret, 0, "WT_SESSION.close");
		goto err;
	}
	ret = cfg->conn->close(cfg->conn, NULL);
	cfg->conn = NULL;
	if (ret != 0) {
		lprintf(cfg, ret, 0, "WT_CONNECTION.close: %s", tmphome);
		goto err;
	}

					/* Sanity-check the configuration */
	if (config_sanity(cfg) != 0)
		goto err;

	if (cfg->verbose > 1)		/* Display the configuration. */
		config_print(cfg);

	if ((ret = wiredtiger_open(	/* Open the real connection. */
	    cfg->home, NULL, cfg->conn_config, &cfg->conn)) != 0) {
		lprintf(cfg, ret, 0, "Error connecting to %s", cfg->home);
		goto err;
	}

	if (cfg->create != 0) {		/* If creating, create the table. */
		if ((ret = cfg->conn->open_session(
		    cfg->conn, NULL, NULL, &session)) != 0) {
			lprintf(cfg, ret, 0,
			    "Error opening a session on %s", cfg->home);
			goto err;
		}
		if ((ret = session->create(
		    session, cfg->uri, cfg->table_config)) != 0) {
			lprintf(cfg,
			    ret, 0, "Error creating table %s", cfg->uri);
			goto err;
		}
		assert(session->close(session, NULL) == 0);
		session = NULL;
	}
					/* Start the monitor thread. */
	if (cfg->sample_interval != 0) {
		if ((ret = pthread_create(
		    &monitor_thread, NULL, monitor, cfg)) != 0) {
			lprintf(
			    cfg, ret, 0, "Error creating monitor thread.");
			goto err;
		}
		monitor_created = 1;
	}
					/* If creating, populate the table. */
	if (cfg->create != 0 && execute_populate(cfg) != 0)
		goto err;
					/* Optional workload. */
	if (cfg->run_time != 0 || cfg->run_ops != 0) {
					/* Didn't create, set insert count. */
		if (cfg->create == 0 && find_table_count(cfg) != 0)
			goto err;
					/* Start the checkpoint thread. */
		if (cfg->checkpoint_threads != 0) {
			lprintf(cfg, 0, 1,
			    "Starting %" PRIu32 " checkpoint thread(s)",
			    cfg->checkpoint_threads);
			if ((cfg->ckptthreads =
			    calloc(cfg->checkpoint_threads,
			    sizeof(CONFIG_THREAD))) == NULL) {
				ret = enomem(cfg);
				goto err;
			}
			if (start_threads(cfg, cfg->ckptthreads, 
			    cfg->checkpoint_threads, checkpoint_worker) != 0)
				goto err;
		}
					/* Execute the workload. */
		if ((ret = execute_workload(cfg)) != 0)
			goto err;

		/* One final summation of the operations we've completed. */
		g_read_ops = sum_read_ops(cfg);
		g_insert_ops = sum_insert_ops(cfg);
		g_update_ops = sum_update_ops(cfg);
		g_ckpt_ops = sum_ckpt_ops(cfg);
		total_ops = g_read_ops + g_insert_ops + g_update_ops;

		lprintf(cfg, 0, 1,
		    "Executed %" PRIu64 " read operations (%" PRIu64 "%%)",
		    g_read_ops, (g_read_ops * 100) / total_ops);
		lprintf(cfg, 0, 1,
		    "Executed %" PRIu64 " insert operations (%" PRIu64 "%%)",
		    g_insert_ops, (g_insert_ops * 100) / total_ops);
		lprintf(cfg, 0, 1,
		    "Executed %" PRIu64 " update operations (%" PRIu64 "%%)",
		    g_update_ops, (g_update_ops * 100) / total_ops);
		lprintf(cfg, 0, 1,
		    "Executed %" PRIu64 " checkpoint operations",
		    g_ckpt_ops);

		latency_print(cfg);
	}

	if (0) {
einval:		ret = EINVAL;
err:		if (ret == 0)
			ret = EXIT_FAILURE;
	}
	if ((tret = stop_threads(cfg, 1, cfg->ckptthreads)) != 0)
		if (ret == 0)
			ret = tret;

	if (monitor_created != 0 &&
	    (tret = pthread_join(monitor_thread, NULL)) != 0) {
		lprintf(cfg, ret, 0, "Error joining monitor thread.");
		if (ret == 0)
			ret = tret;
	}

	if (cfg->conn != NULL &&
	    (tret = cfg->conn->close(cfg->conn, NULL)) != 0) {
		lprintf(cfg, ret, 0,
		    "Error closing connection to %s", cfg->home);
		if (ret == 0)
			ret = tret;
	}

	if (ret == 0)
		lprintf(cfg, 0, 1, "Run completed: %" PRIu32 " %s",
		    cfg->run_time == 0 ? cfg->run_ops : cfg->run_time,
		    cfg->run_time == 0 ? "operations" : "seconds");

	if (cfg->logf != NULL) {
		assert(fflush(cfg->logf) == 0);
		assert(fclose(cfg->logf) == 0);
	}
	free(cfg->popthreads);
	free(cfg->workers);
	config_free(cfg);

	free(cc_buf);
	free(cmd);
	free(tc_buf);
	free(tmphome);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int
start_threads(
    CONFIG *cfg, CONFIG_THREAD *thread, u_int num, void *(*func)(void *))
{
	u_int i;
	int ret;

	for (i = 0; i < num; ++i, ++thread) {
		thread->cfg = cfg;

		/*
		 * Every thread gets a key/data buffer because we don't bother
		 * to distinguish between threads needing them and threads that
		 * don't, it's not enough memory to bother.
		 */
		if ((thread->key_buf = calloc(cfg->key_sz + 1, 1)) == NULL)
			return (enomem(cfg));
		if ((thread->value_buf = calloc(cfg->data_sz, 1)) == NULL)
			return (enomem(cfg));
		memset(thread->value_buf, 'a', cfg->data_sz - 1);

		/*
		 * Every thread gets tracking information and is initialized
		 * for latency measurements, for the same reason.
		 */
		thread->ckpt.min_latency =
		thread->insert.min_latency = thread->read.min_latency =
		thread->update.min_latency = UINT32_MAX;
		thread->ckpt.max_latency = thread->insert.max_latency =
		thread->read.max_latency = thread->update.max_latency = 0;

		if ((ret = pthread_create(
		    &thread->handle, NULL, func, thread)) != 0) {
			lprintf(cfg, ret, 0, "Error creating thread");
			return (ret);
		}
	}
	return (0);
}

static int
stop_threads(CONFIG *cfg, u_int num, CONFIG_THREAD *threads)
{
	u_int i;
	int ret;

	if (num == 0 || threads == NULL)
		return (0);

	for (i = 0; i < num; ++i, ++threads) {
		if ((ret = pthread_join(threads->handle, NULL)) != 0) {
			lprintf(cfg, ret, 0, "Error joining thread");
			return (ret);
		}

		free(threads->key_buf);
		threads->key_buf = NULL;
		free(threads->value_buf);
		threads->value_buf = NULL;
	}

	/*
	 * We don't free the thread structures or any memory referenced, or NULL
	 * the reference when we stop the threads; the thread structure is still
	 * being read by the monitor thread (among others).  As a standalone
	 * program, leaking memory isn't a concern, and it's simpler that way.
	 */
	return (0);
}

static uint64_t
wtperf_value_range(CONFIG *cfg)
{
	if (cfg->random_range)
		return (cfg->icount + cfg->random_range);
	else
		return (cfg->icount + g_insert_key - (cfg->insert_threads + 1));
}

static uint64_t
wtperf_rand(CONFIG *cfg)
{
	double S1, S2, U;
	uint64_t rval;

	/*
	 * Use WiredTiger's random number routine: it's lock-free and fairly
	 * good.
	 */
	rval = (uint64_t)__wt_random();

	/* Use Pareto distribution to give 80/20 hot/cold values. */
	if (cfg->pareto) {
#define	PARETO_SHAPE	1.5
		S1 = (-1 / PARETO_SHAPE);
		S2 = wtperf_value_range(cfg) * 0.2 * (PARETO_SHAPE - 1);
		U = 1 - (double)rval / (double)RAND_MAX;
		rval = (pow(U, S1) - 1) * S2;
		/*
		 * This Pareto calculation chooses out of range values about
		 * about 2% of the time, from my testing. That will lead to the
		 * last item in the table being "hot".
		 */
		if (rval > wtperf_value_range(cfg))
			rval = wtperf_value_range(cfg);
	}
	/* Avoid zero - LSM doesn't like it. */
	rval = (rval % wtperf_value_range(cfg)) + 1;
	return (rval);
}
