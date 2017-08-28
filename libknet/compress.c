/*
 * Copyright (C) 2010-2017 Red Hat, Inc.  All rights reserved.
 *
 * Author: Fabio M. Di Nitto <fabbione@kronosnet.org>
 *
 * This software licensed under GPL-2.0+, LGPL-2.0+
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "internals.h"
#include "compress.h"
#include "logging.h"
#include "threads_common.h"

#ifdef BUILDCOMPZLIB
#include "compress_zlib.h"
#endif
#ifdef BUILDCOMPLZ4
#include "compress_lz4.h"
#endif
#ifdef BUILDCOMPLZO2
#include "compress_lzo2.h"
#endif
#ifdef BUILDCOMPLZMA
#include "compress_lzma.h"
#endif
#ifdef BUILDCOMPBZIP2
#include "compress_bzip2.h"
#endif

/*
 * internal module switch data
 */

/*
 * DO NOT CHANGE MODEL_ID HERE OR ONWIRE COMPATIBILITY
 * WILL BREAK!
 *
 * always add before the last NULL/NULL/NULL.
 */

#define empty_module 0, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL },

compress_model_t compress_modules_cmds[] = {
	{ "none", 0, empty_module
	{ "zlib", 1,
#ifdef BUILDCOMPZLIB
		     1, zlib_load_lib, zlib_unload_lib, 0, 0, NULL, NULL, NULL, zlib_val_level, zlib_compress, zlib_decompress },
#else
empty_module
#endif
	{ "lz4", 2,
#ifdef BUILDCOMPLZ4
		     1, lz4_load_lib, lz4_unload_lib, 0, 0, NULL, NULL, NULL, lz4_val_level, lz4_compress, lz4_decompress },
#else
empty_module
#endif
	{ "lz4hc", 3,
#ifdef BUILDCOMPLZ4
		     1, lz4_load_lib, lz4_unload_lib, 0, 0, NULL, NULL, NULL, lz4hc_val_level, lz4hc_compress, lz4_decompress },
#else
empty_module
#endif
	{ "lzo2", 4,
#ifdef BUILDCOMPLZO2
		     1, lzo2_load_lib, lzo2_unload_lib, 0, 0, lzo2_is_init, lzo2_init, lzo2_fini, lzo2_val_level, lzo2_compress, lzo2_decompress },
#else
empty_module
#endif
	{ "lzma", 5,
#ifdef BUILDCOMPLZMA
		     1, lzma_load_lib, lzma_unload_lib, 0, 0, NULL, NULL, NULL, lzma_val_level, lzma_compress, lzma_decompress },
#else
empty_module
#endif
	{ "bzip2", 6,
#ifdef BUILDCOMPBZIP2
		     1, bzip2_load_lib, bzip2_unload_lib, 0, 0, NULL, NULL, NULL, bzip2_val_level, bzip2_compress, bzip2_decompress },
#else
empty_module
#endif
	{ NULL, 255, empty_module
};

static int max_model = 0;
static struct timespec last_load_failure;

static int get_model(const char *model)
{
	int idx = 0;

	while (compress_modules_cmds[idx].model_name != NULL) {
		if (!strcmp(compress_modules_cmds[idx].model_name, model)) {
			return compress_modules_cmds[idx].model_id;
		}
		idx++;
	}
	return -1;
}

static int get_max_model(void)
{
	int idx = 0;
	while (compress_modules_cmds[idx].model_name != NULL) {
		idx++;
	}
	return idx - 1;
}

static int is_valid_model(int compress_model)
{
	int idx = 0;

	while (compress_modules_cmds[idx].model_name != NULL) {
		if ((compress_model == compress_modules_cmds[idx].model_id) &&
		    (compress_modules_cmds[idx].built_in == 1)) {
			return 0;
		}
		idx++;
	}
	return -1;
}

static int val_level(
	knet_handle_t knet_h,
	int compress_model,
	int compress_level)
{
	return compress_modules_cmds[compress_model].val_level(knet_h, compress_level);
}

static int check_init_lib(knet_handle_t knet_h, int cmp_model, int rate_limit)
{
	struct timespec clock_now;
	unsigned long long timediff;
	int savederrno = 0;

	savederrno = pthread_rwlock_rdlock(&shlib_rwlock);
	if (savederrno) {
		log_err(knet_h, KNET_SUB_COMPRESS, "Unable to get read lock: %s",
			strerror(savederrno));
		errno = savederrno;
		return -1;
	}

	/*
	 * if the module is already loaded and init for this handle,
	 * we will return and keep the lock to avoid any race condition
	 * on other threads potentially unloading or reloading.
	 *
	 * lack of a .is_init function means that the module does not require
	 * init per handle so we use a fake reference in the compress_int_data
	 * to identify that we already increased the libref for this handle
	 */
	if (compress_modules_cmds[cmp_model].loaded == 1) {
		if (compress_modules_cmds[cmp_model].is_init == NULL) {
			if (knet_h->compress_int_data[cmp_model] != NULL) {
				return 0;
			}
		} else {
			if (compress_modules_cmds[cmp_model].is_init(knet_h, cmp_model) == 1) {
				return 0;
			}
		}
	}

	/*
	 * due to the fact that decompress can load libraries
	 * on demand, depending on the compress model selected
	 * on other nodes, it is possible for an attacker
	 * to send crafted packets to attempt to load libraries
	 * at random in a DoS fashion.
	 * If there is an error loading a library, then we want
	 * to rate_limit a retry to reload the library every X
	 * seconds to avoid a lock DoS that could greatly slow
	 * down libknet.
	 */
	if (rate_limit) {
		if ((last_load_failure.tv_sec != 0) ||
		    (last_load_failure.tv_nsec != 0)) {
			clock_gettime(CLOCK_MONOTONIC, &clock_now);
			timespec_diff(last_load_failure, clock_now, &timediff);
			if (timediff < 10000000000) {
				pthread_rwlock_unlock(&shlib_rwlock);
				errno = EAGAIN;
				return -1;
			}
		}
	}

	/*
	 * need to switch to write lock, load the lib, and return with a write lock
	 * this is not racy because .init should be written idempotent.
	 */
	pthread_rwlock_unlock(&shlib_rwlock);
	savederrno = pthread_rwlock_wrlock(&shlib_rwlock);
	if (savederrno) {
		log_err(knet_h, KNET_SUB_COMPRESS, "Unable to get write lock: %s",
			strerror(savederrno));
		errno = savederrno;
		return -1;
	}

	if (compress_modules_cmds[cmp_model].loaded == 0) {
		if (compress_modules_cmds[cmp_model].load_lib(knet_h) < 0) {
			clock_gettime(CLOCK_MONOTONIC, &last_load_failure);
			pthread_rwlock_unlock(&shlib_rwlock);
			return -1;
		}
		compress_modules_cmds[cmp_model].loaded = 1;
	}

	if (compress_modules_cmds[cmp_model].init != NULL) {
		if (compress_modules_cmds[cmp_model].init(knet_h, cmp_model) < 0) {
			pthread_rwlock_unlock(&shlib_rwlock);
			return -1;
		}
	} else {
		knet_h->compress_int_data[cmp_model] = &"1";
	}

	compress_modules_cmds[cmp_model].libref++;

	return 0;
}

int compress_init(
	knet_handle_t knet_h)
{
	max_model = get_max_model();
	if (max_model > KNET_MAX_COMPRESS_METHODS) {
		log_err(knet_h, KNET_SUB_COMPRESS, "Too many compress methods defined in compress.c.");
		errno = EINVAL;
		return -1;
	}

	memset(&last_load_failure, 0, sizeof(struct timespec));

	return 0;
}

int compress_cfg(
	knet_handle_t knet_h,
	struct knet_handle_compress_cfg *knet_handle_compress_cfg)
{
	int savederrno = 0, err = 0;
	int cmp_model;

	log_debug(knet_h, KNET_SUB_COMPRESS,
		  "Initizializing compress module [%s/%d/%u]",
		  knet_handle_compress_cfg->compress_model, knet_handle_compress_cfg->compress_level, knet_handle_compress_cfg->compress_threshold);

	cmp_model = get_model(knet_handle_compress_cfg->compress_model);
	if (cmp_model < 0) {
		log_err(knet_h, KNET_SUB_COMPRESS, "compress model %s not supported", knet_handle_compress_cfg->compress_model);
		errno = EINVAL;
		return -1;
	}

	if (cmp_model > 0) {
		if (compress_modules_cmds[cmp_model].built_in == 0) {
			log_err(knet_h, KNET_SUB_COMPRESS, "compress model %s support has not been built in. Please contact your vendor or fix the build", knet_handle_compress_cfg->compress_model);
			savederrno = EINVAL;
			err = -1;
			goto out;
		}

		if (check_init_lib(knet_h, cmp_model, 0) < 0) {
			savederrno = errno;
			log_err(knet_h, KNET_SUB_COMPRESS, "Unable to load/init shared lib for model %s: %s",
				knet_handle_compress_cfg->compress_model, strerror(errno));
			err = -1;
			goto out_unlock;
		}

		if (val_level(knet_h, cmp_model, knet_handle_compress_cfg->compress_level) < 0) {
			log_err(knet_h, KNET_SUB_COMPRESS, "compress level %d not supported for model %s",
				knet_handle_compress_cfg->compress_level, knet_handle_compress_cfg->compress_model);
			savederrno = EINVAL;
			err = -1;
			goto out_unlock;
		}

		if (knet_handle_compress_cfg->compress_threshold > KNET_MAX_PACKET_SIZE) {
			log_err(knet_h, KNET_SUB_COMPRESS, "compress threshold cannot be higher than KNET_MAX_PACKET_SIZE (%d).",
				 KNET_MAX_PACKET_SIZE);
			savederrno = EINVAL;
			err = -1;
			goto out_unlock;
		}
		if (knet_handle_compress_cfg->compress_threshold == 0) {
			knet_h->compress_threshold = KNET_COMPRESS_THRESHOLD;
			log_debug(knet_h, KNET_SUB_COMPRESS, "resetting compression threshold to default (%d)", KNET_COMPRESS_THRESHOLD);
		} else {
			knet_h->compress_threshold = knet_handle_compress_cfg->compress_threshold;
		}
out_unlock:
		pthread_rwlock_unlock(&shlib_rwlock);
	}
out:
	if (!err) {
		knet_h->compress_model = cmp_model;
		knet_h->compress_level = knet_handle_compress_cfg->compress_level;
	}

	errno = savederrno;
	return err;
}

void compress_fini(
	knet_handle_t knet_h)
{
	int savederrno = 0;
	int idx = 0;

	savederrno = pthread_rwlock_wrlock(&shlib_rwlock);
	if (savederrno) {
		log_err(knet_h, KNET_SUB_COMPRESS, "Unable to get write lock: %s",
			strerror(savederrno));
		return;
	}

	while (compress_modules_cmds[idx].model_name != NULL) {
		if ((compress_modules_cmds[idx].built_in == 1) &&
		    (compress_modules_cmds[idx].loaded == 1) &&
		    (idx < KNET_MAX_COMPRESS_METHODS)) {
			if (compress_modules_cmds[idx].fini != NULL) {
				compress_modules_cmds[idx].fini(knet_h, idx);
			} else {
				knet_h->compress_int_data[idx] = NULL;
			}
			compress_modules_cmds[idx].libref--;

			if ((compress_modules_cmds[idx].libref == 0) &&
			    (compress_modules_cmds[idx].loaded == 1)) {
				log_debug(knet_h, KNET_SUB_COMPRESS, "Unloading %s library", compress_modules_cmds[idx].model_name);
				compress_modules_cmds[idx].unload_lib(knet_h);
				compress_modules_cmds[idx].loaded = 0;
			}
		}
		idx++;
	}

	pthread_rwlock_unlock(&shlib_rwlock);
	return;
}

int compress(
	knet_handle_t knet_h,
	const unsigned char *buf_in,
	const ssize_t buf_in_len,
	unsigned char *buf_out,
	ssize_t *buf_out_len)
{
	int savederrno = 0, err = 0;

	if (check_init_lib(knet_h, knet_h->compress_model, 0) < 0) {
		savederrno = errno;
		log_err(knet_h, KNET_SUB_COMPRESS, "Unable to load/init shared lib (compress) for model %s: %s",
			compress_modules_cmds[knet_h->compress_model].model_name, strerror(savederrno));
		return -1;
	}

	err = compress_modules_cmds[knet_h->compress_model].compress(knet_h, buf_in, buf_in_len, buf_out, buf_out_len);
	savederrno = errno;

	pthread_rwlock_unlock(&shlib_rwlock);

	errno = savederrno;
	return err;
}

int decompress(
	knet_handle_t knet_h,
	int compress_model,
	const unsigned char *buf_in,
	const ssize_t buf_in_len,
	unsigned char *buf_out,
	ssize_t *buf_out_len)
{
	int savederrno = 0, err = 0;

	if (compress_model > max_model) {
		log_err(knet_h,  KNET_SUB_COMPRESS, "Received packet with unknown compress model %d", compress_model);
		errno = EINVAL;
		return -1;
	}

	if (is_valid_model(compress_model) < 0) {
		log_err(knet_h,  KNET_SUB_COMPRESS, "Received packet compressed with %s but support is not built in this version of libknet. Please contact your distribution vendor or fix the build.", compress_modules_cmds[compress_model].model_name);
		errno = EINVAL;
		return -1;
	}

	if (check_init_lib(knet_h, compress_model, 1) < 0) {
		savederrno = errno;
		log_err(knet_h, KNET_SUB_COMPRESS, "Unable to load/init shared lib (decompress) for model %s: %s",
			compress_modules_cmds[compress_model].model_name, strerror(savederrno));
		return -1;
	}

	err = compress_modules_cmds[compress_model].decompress(knet_h, buf_in, buf_in_len, buf_out, buf_out_len);
	savederrno = errno;

	pthread_rwlock_unlock(&shlib_rwlock);

	errno = savederrno;
	return err;
}
