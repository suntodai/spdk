/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * Block device abstraction layer
 */

#ifndef SPDK_BDEV_H_
#define SPDK_BDEV_H_

#include <inttypes.h>
#include <unistd.h>
#include <stddef.h>  /* for offsetof */
#include <sys/uio.h> /* for struct iovec */
#include <stdbool.h>

#include "spdk/event.h"
#include "spdk/queue.h"
#include "spdk/scsi_spec.h"

#define SPDK_BDEV_SMALL_RBUF_MAX_SIZE 8192
#define SPDK_BDEV_LARGE_RBUF_MAX_SIZE (64 * 1024)

#define SPDK_BDEV_MAX_NAME_LENGTH		16
#define SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH	50

struct spdk_bdev_io;

/** \page block_backend_modules Block Device Backend Modules

To implement a backend block device driver, a number of functions
dictated by struct spdk_bdev_fn_table must be provided.

The module should register itself using SPDK_BDEV_MODULE_REGISTER or
SPDK_VBDEV_MODULE_REGISTER to define the parameters for the module.

Use SPDK_BDEV_MODULE_REGISTER for all block backends that are real disks.
Any virtual backends such as RAID, partitioning, etc. should use
SPDK_VBDEV_MODULE_REGISTER.

<hr>

In the module initialization code, the config file sections can be parsed to
acquire custom configuration parameters. For example, if the config file has
a section such as below:
<blockquote><pre>
[MyBE]
  MyParam 1234
</pre></blockquote>

The value can be extracted as the example below:
<blockquote><pre>
struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "MyBe");
int my_param = spdk_conf_section_get_intval(sp, "MyParam");
</pre></blockquote>

The backend initialization routine also need to create "disks". A virtual
representation of each LUN must be constructed. Mainly a struct spdk_bdev
must be passed to the bdev database via spdk_bdev_register().

*/

/**
 * \brief SPDK block device.
 *
 * This is a virtual representation of a block device that is exported by the backend.
 */
struct spdk_bdev {
	/** User context passed in by the backend */
	void *ctxt;

	/** Unique name for this block device. */
	char name[SPDK_BDEV_MAX_NAME_LENGTH];

	/** Unique product name for this kind of block device. */
	char product_name[SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH];

	/** Size in bytes of a logical block for the backend */
	uint32_t blocklen;

	/** Number of blocks */
	uint64_t blockcnt;

	/** write cache enabled, not used at the moment */
	int write_cache;

	/**
	 * This is used to make sure buffers are sector aligned.
	 * This causes double buffering on writes.
	 */
	int need_aligned_buffer;

	/** thin provisioning, not used at the moment */
	int thin_provisioning;

	/** function table for all LUN ops */
	struct spdk_bdev_fn_table *fn_table;

	/** Represents maximum unmap block descriptor count */
	uint32_t max_unmap_bdesc_count;

	/** array of child block dev that is underneath of the current dev */
	struct spdk_bdev **child_bdevs;

	/** number of child blockdevs allocated */
	int num_child_bdevs;

	/** generation value used by block device reset */
	uint32_t gencnt;

	/** Whether the poller is registered with the reactor */
	bool is_running;

	/** Which lcore the poller is running on */
	uint32_t lcore;

	/** Poller to submit IO and check completion */
	struct spdk_poller *poller;

	/** True if another blockdev or a LUN is using this device */
	bool claimed;

	TAILQ_ENTRY(spdk_bdev) link;
};

/**
 * Function table for a block device backend.
 *
 * The backend block device function table provides a set of APIs to allow
 * communication with a backend. The main commands are read/write API
 * calls for I/O via submit_request.
 */
struct spdk_bdev_fn_table {
	/** Destroy the backend block device object */
	int (*destruct)(struct spdk_bdev *bdev);

	/** Poll the backend for I/O waiting to be completed. */
	int (*check_io)(struct spdk_bdev *bdev);

	/** Process the IO. */
	void (*submit_request)(struct spdk_bdev_io *);

	/** Release buf for read command. */
	void (*free_request)(struct spdk_bdev_io *);
};

/** Blockdev I/O type */
enum spdk_bdev_io_type {
	SPDK_BDEV_IO_TYPE_INVALID,
	SPDK_BDEV_IO_TYPE_READ,
	SPDK_BDEV_IO_TYPE_WRITE,
	SPDK_BDEV_IO_TYPE_UNMAP,
	SPDK_BDEV_IO_TYPE_FLUSH,
	SPDK_BDEV_IO_TYPE_RESET,
};

/** Blockdev I/O completion status */
enum spdk_bdev_io_status {
	SPDK_BDEV_IO_STATUS_FAILED = -1,
	SPDK_BDEV_IO_STATUS_PENDING = 0,
	SPDK_BDEV_IO_STATUS_SUCCESS = 1,
};

/** Blockdev reset operation type */
enum spdk_bdev_reset_type {
	/**
	 * A hard reset indicates that the blockdev layer should not
	 *  invoke the completion callback for I/Os issued before the
	 *  reset is issued but completed after the reset is complete.
	 */
	SPDK_BDEV_RESET_HARD,

	/**
	 * A soft reset indicates that the blockdev layer should still
	 *  invoke the completion callback for I/Os issued before the
	 *  reset is issued but completed after the reset is complete.
	 */
	SPDK_BDEV_RESET_SOFT,
};

typedef spdk_event_fn spdk_bdev_io_completion_cb;
typedef void (*spdk_bdev_io_get_rbuf_cb)(struct spdk_bdev_io *bdev_io);

/**
 * Block device I/O
 *
 * This is an I/O that is passed to an spdk_bdev.
 */
struct spdk_bdev_io {
	/** Pointer to scratch area reserved for use by the driver consuming this spdk_bdev_io. */
	void *ctx;

	/** Generation value for each I/O. */
	uint32_t gencnt;

	/** The block device that this I/O belongs to. */
	struct spdk_bdev *bdev;

	/** Enumerated value representing the I/O type. */
	enum spdk_bdev_io_type type;

	union {
		struct {

			/** The unaligned rbuf originally allocated. */
			void *buf_unaligned;

			/** For single buffer cases, pointer to the aligned data buffer.  */
			void *buf;

			/** For single buffer cases, size of the data buffer. */
			uint64_t nbytes;

			/** Starting offset (in bytes) of the blockdev for this I/O. */
			uint64_t offset;

			/** Indicate whether the blockdev layer to put rbuf or not. */
			bool put_rbuf;
		} read;
		struct {
			/** For basic write case, use our own iovec element */
			struct iovec iov;

			/** For SG buffer cases, array of iovecs to transfer. */
			struct iovec *iovs;

			/** For SG buffer cases, number of iovecs in iovec array. */
			int iovcnt;

			/** For SG buffer cases, total size of data to be transferred. */
			size_t len;

			/** Starting offset (in bytes) of the blockdev for this I/O. */
			uint64_t offset;
		} write;
		struct {
			/** Represents the unmap block descriptors. */
			struct spdk_scsi_unmap_bdesc *unmap_bdesc;

			/** Count of unmap block descriptors. */
			uint16_t bdesc_count;
		} unmap;
		struct {
			/** Represents starting offset in bytes of the range to be flushed. */
			uint64_t offset;

			/** Represents the number of bytes to be flushed, starting at offset. */
			uint64_t length;
		} flush;
		struct {
			int32_t type;
		} reset;
	} u;

	/** User function that will be called when this completes */
	spdk_bdev_io_completion_cb cb;

	/** Context that will be passed to the completion callback */
	void *caller_ctx;

	struct spdk_event *cb_event;

	/** Callback for when rbuf is allocated */
	spdk_bdev_io_get_rbuf_cb get_rbuf_cb;

	/** Status for the IO */
	enum spdk_bdev_io_status status;

	/** Used in virtual device (e.g., RAID), indicates its parent spdk_bdev_io **/
	void *parent;

	/** Used in virtual device (e.g., RAID) for storing multiple child device I/Os **/
	TAILQ_HEAD(child_io, spdk_bdev_io) child_io;

	/** Member used for linking child I/Os together. */
	TAILQ_ENTRY(spdk_bdev_io) link;

	/** Number of children for this I/O */
	int children;

	/** Entry to the list need_buf of struct spdk_bdev. */
	TAILQ_ENTRY(spdk_bdev_io) rbuf_link;

	/** Per I/O context for use by the blockdev module */
	uint8_t driver_ctx[0];

	/* No members may be added after driver_ctx! */
};

/** Block device module */
struct spdk_bdev_module_if {
	/**
	 * Initialization function for the module.  Called by the spdk
	 * application during startup.
	 *
	 * Modules are required to define this function.
	 */
	int (*module_init)(void);

	/**
	 * Finish function for the module.  Called by the spdk application
	 * before the spdk application exits to perform any necessary cleanup.
	 *
	 * Modules are not required to define this function.
	 */
	void (*module_fini)(void);

	/**
	 * Function called to return a text string representing the
	 * module's configuration options for inclusion in a configuration file.
	 */
	void (*config_text)(FILE *fp);

	/** Name for the modules being defined. */
	const char *module_name;

	/**
	 * Returns the allocation size required for the backend for uses such as local
	 * command structs, local SGL, iovecs, or other user context.
	 */
	int (*get_ctx_size)(void);

	TAILQ_ENTRY(spdk_bdev_module_if) tailq;
};

/* The blockdev API has two distinct parts. The first portion of the API
 * is to be used by the layer above the blockdev in order to communicate
 * with it. The second portion of the API is to be used by the blockdev
 * modules themselves to perform operations like completing I/O.
 */

/* The following functions are intended to be called from the upper layer
 * that is using the blockdev layer.
 */

struct spdk_bdev *spdk_bdev_get_by_name(const char *bdev_name);

struct spdk_bdev *spdk_bdev_first(void);
struct spdk_bdev *spdk_bdev_next(struct spdk_bdev *prev);

struct spdk_bdev_io *spdk_bdev_read(struct spdk_bdev *bdev,
				    void *buf, uint64_t nbytes, uint64_t offset,
				    spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *spdk_bdev_write(struct spdk_bdev *bdev,
				     void *buf, uint64_t nbytes, uint64_t offset,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *spdk_bdev_writev(struct spdk_bdev *bdev,
				      struct iovec *iov, int iovcnt,
				      uint64_t len, uint64_t offset,
				      spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *spdk_bdev_unmap(struct spdk_bdev *bdev,
				     struct spdk_scsi_unmap_bdesc *unmap_d,
				     uint16_t bdesc_count,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *spdk_bdev_flush(struct spdk_bdev *bdev,
				     uint64_t offset, uint64_t length,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);
int spdk_bdev_io_submit(struct spdk_bdev_io *bdev_io);
void spdk_bdev_do_work(void *ctx);
int spdk_bdev_reset(struct spdk_bdev *bdev, int reset_type,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/* The remaining functions are intended to be called from within
 * blockdev modules.
 */
void spdk_bdev_register(struct spdk_bdev *bdev);
void spdk_bdev_unregister(struct spdk_bdev *bdev);
int spdk_bdev_free_io(struct spdk_bdev_io *bdev_io);
void spdk_bdev_io_get_rbuf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_rbuf_cb cb);
struct spdk_bdev_io *spdk_bdev_get_io(void);
struct spdk_bdev_io *spdk_bdev_get_child_io(struct spdk_bdev_io *parent,
		struct spdk_bdev *bdev,
		spdk_bdev_io_completion_cb cb,
		void *cb_arg);
void spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io,
			   enum spdk_bdev_io_status status);
void spdk_bdev_module_list_add(struct spdk_bdev_module_if *bdev_module);
void spdk_vbdev_module_list_add(struct spdk_bdev_module_if *vbdev_module);

static inline struct spdk_bdev_io *
spdk_bdev_io_from_ctx(void *ctx)
{
	return (struct spdk_bdev_io *)
	       ((uintptr_t)ctx - offsetof(struct spdk_bdev_io, driver_ctx));
}

#define SPDK_BDEV_MODULE_REGISTER(init_fn, fini_fn, config_fn, ctx_size_fn)			\
	static struct spdk_bdev_module_if init_fn ## _if = {					\
	.module_init 	= init_fn,								\
	.module_fini	= fini_fn,								\
	.config_text	= config_fn,								\
	.get_ctx_size	= ctx_size_fn,                                				\
	};  											\
	__attribute__((constructor)) static void init_fn ## _init(void)  			\
	{                                                           				\
	    spdk_bdev_module_list_add(&init_fn ## _if);                  			\
	}

#define SPDK_VBDEV_MODULE_REGISTER(init_fn, fini_fn, config_fn, ctx_size_fn)			\
	static struct spdk_bdev_module_if init_fn ## _if = {					\
	.module_init 	= init_fn,								\
	.module_fini	= fini_fn,								\
	.config_text	= config_fn,								\
	.get_ctx_size	= ctx_size_fn,                                				\
	};  											\
	__attribute__((constructor)) static void init_fn ## _init(void)  			\
	{                                                           				\
	    spdk_vbdev_module_list_add(&init_fn ## _if);                  			\
	}

#endif /* SPDK_BDEV_H_ */
