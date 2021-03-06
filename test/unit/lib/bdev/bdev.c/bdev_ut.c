/*-
 *   BSD LICENSE
 *
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

#include "spdk_cunit.h"

#include "common/lib/test_env.c"
#include "unit/lib/json_mock.c"

/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"

DEFINE_STUB(spdk_conf_find_section, struct spdk_conf_section *, (struct spdk_conf *cp,
		const char *name), NULL);
DEFINE_STUB(spdk_conf_section_get_nmval, char *,
	    (struct spdk_conf_section *sp, const char *key, int idx1, int idx2), NULL);
DEFINE_STUB(spdk_conf_section_get_intval, int, (struct spdk_conf_section *sp, const char *key), -1);

static void
_bdev_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

void
spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			 int *sc, int *sk, int *asc, int *ascq)
{
}

static int
null_init(void)
{
	return 0;
}

static int
null_clean(void)
{
	return 0;
}

static int
stub_destruct(void *ctx)
{
	return 0;
}

struct bdev_ut_channel {
	TAILQ_HEAD(, spdk_bdev_io)	outstanding_io;
	uint32_t			outstanding_io_count;
};

static uint32_t g_bdev_ut_io_device;
static struct bdev_ut_channel *g_bdev_ut_channel;

static void
stub_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_ut_channel *ch = spdk_io_channel_get_ctx(_ch);

	TAILQ_INSERT_TAIL(&ch->outstanding_io, bdev_io, module_link);
	ch->outstanding_io_count++;
}

static uint32_t
stub_complete_io(uint32_t num_to_complete)
{
	struct bdev_ut_channel *ch = g_bdev_ut_channel;
	struct spdk_bdev_io *bdev_io;
	uint32_t num_completed = 0;

	while (num_completed < num_to_complete) {
		if (TAILQ_EMPTY(&ch->outstanding_io)) {
			break;
		}
		bdev_io = TAILQ_FIRST(&ch->outstanding_io);
		TAILQ_REMOVE(&ch->outstanding_io, bdev_io, module_link);
		ch->outstanding_io_count--;
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		num_completed++;
	}

	return num_completed;
}

static struct spdk_io_channel *
bdev_ut_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_bdev_ut_io_device);
}

static struct spdk_bdev_fn_table fn_table = {
	.destruct = stub_destruct,
	.submit_request = stub_submit_request,
	.get_io_channel = bdev_ut_get_io_channel,
};

static int
bdev_ut_create_ch(void *io_device, void *ctx_buf)
{
	struct bdev_ut_channel *ch = ctx_buf;

	CU_ASSERT(g_bdev_ut_channel == NULL);
	g_bdev_ut_channel = ch;

	TAILQ_INIT(&ch->outstanding_io);
	ch->outstanding_io_count = 0;
	return 0;
}

static void
bdev_ut_destroy_ch(void *io_device, void *ctx_buf)
{
	CU_ASSERT(g_bdev_ut_channel != NULL);
	g_bdev_ut_channel = NULL;
}

static int
bdev_ut_module_init(void)
{
	spdk_io_device_register(&g_bdev_ut_io_device, bdev_ut_create_ch, bdev_ut_destroy_ch,
				sizeof(struct bdev_ut_channel));
	return 0;
}

static void
bdev_ut_module_fini(void)
{
}

struct spdk_bdev_module bdev_ut_if = {
	.name = "bdev_ut",
	.module_init = bdev_ut_module_init,
	.module_fini = bdev_ut_module_fini,
};

static void vbdev_ut_examine(struct spdk_bdev *bdev);

static int
vbdev_ut_module_init(void)
{
	return 0;
}

static void
vbdev_ut_module_fini(void)
{
}

struct spdk_bdev_module vbdev_ut_if = {
	.name = "vbdev_ut",
	.examine = vbdev_ut_examine,
	.module_init = vbdev_ut_module_init,
	.module_fini = vbdev_ut_module_fini,
};

SPDK_BDEV_MODULE_REGISTER(&bdev_ut_if)
SPDK_BDEV_MODULE_REGISTER(&vbdev_ut_if)

static void
vbdev_ut_examine(struct spdk_bdev *bdev)
{
	spdk_bdev_module_examine_done(&vbdev_ut_if);
}

static bool
is_vbdev(struct spdk_bdev *base, struct spdk_bdev *vbdev)
{
	size_t i;
	int found = 0;

	for (i = 0; i < base->vbdevs_cnt; i++) {
		found += base->vbdevs[i] == vbdev;
	}

	CU_ASSERT(found <= 1);
	return !!found;
}

static bool
is_base_bdev(struct spdk_bdev *base, struct spdk_bdev *vbdev)
{
	size_t i;
	int found = 0;

	for (i = 0; i < vbdev->internal.base_bdevs_cnt; i++) {
		found += vbdev->internal.base_bdevs[i] == base;
	}

	CU_ASSERT(found <= 1);
	return !!found;
}

static bool
check_base_and_vbdev(struct spdk_bdev *base, struct spdk_bdev *vbdev)
{
	bool _is_vbdev = is_vbdev(base, vbdev);
	bool _is_base = is_base_bdev(base, vbdev);

	CU_ASSERT(_is_vbdev == _is_base);

	return _is_base && _is_vbdev;
}

static struct spdk_bdev *
allocate_bdev(char *name)
{
	struct spdk_bdev *bdev;
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = &bdev_ut_if;
	bdev->blockcnt = 1;

	rc = spdk_bdev_register(bdev);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.base_bdevs_cnt == 0);
	CU_ASSERT(bdev->vbdevs_cnt == 0);

	return bdev;
}

static struct spdk_bdev *
allocate_vbdev(char *name, struct spdk_bdev *base1, struct spdk_bdev *base2)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev *array[2];
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = &vbdev_ut_if;

	/* vbdev must have at least one base bdev */
	CU_ASSERT(base1 != NULL);

	array[0] = base1;
	array[1] = base2;

	rc = spdk_vbdev_register(bdev, array, base2 == NULL ? 1 : 2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev->internal.base_bdevs_cnt > 0);
	CU_ASSERT(bdev->vbdevs_cnt == 0);

	CU_ASSERT(check_base_and_vbdev(base1, bdev) == true);

	if (base2) {
		CU_ASSERT(check_base_and_vbdev(base2, bdev) == true);
	}

	return bdev;
}

static void
free_bdev(struct spdk_bdev *bdev)
{
	spdk_bdev_unregister(bdev, NULL, NULL);
	memset(bdev, 0xFF, sizeof(*bdev));
	free(bdev);
}

static void
free_vbdev(struct spdk_bdev *bdev)
{
	CU_ASSERT(bdev->internal.base_bdevs_cnt != 0);
	spdk_bdev_unregister(bdev, NULL, NULL);
	memset(bdev, 0xFF, sizeof(*bdev));
	free(bdev);
}

static void
get_device_stat_cb(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat, void *cb_arg, int rc)
{
	const char *bdev_name;

	CU_ASSERT(bdev != NULL);
	CU_ASSERT(rc == 0);
	bdev_name = spdk_bdev_get_name(bdev);
	CU_ASSERT_STRING_EQUAL(bdev_name, "bdev0");

	free(stat);
	free_bdev(bdev);
}

static void
get_device_stat_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_io_stat *stat;

	bdev = allocate_bdev("bdev0");
	stat = calloc(1, sizeof(struct spdk_bdev_io_stat));
	if (stat == NULL) {
		free_bdev(bdev);
		return;
	}
	spdk_bdev_get_device_stat(bdev, stat, get_device_stat_cb, NULL);
}

static void
open_write_test(void)
{
	struct spdk_bdev *bdev[9];
	struct spdk_bdev_desc *desc[9] = {};
	int rc;

	/*
	 * Create a tree of bdevs to test various open w/ write cases.
	 *
	 * bdev0 through bdev3 are physical block devices, such as NVMe
	 * namespaces or Ceph block devices.
	 *
	 * bdev4 is a virtual bdev with multiple base bdevs.  This models
	 * caching or RAID use cases.
	 *
	 * bdev5 through bdev7 are all virtual bdevs with the same base
	 * bdev (except bdev7). This models partitioning or logical volume
	 * use cases.
	 *
	 * bdev7 is a virtual bdev with multiple base bdevs. One of base bdevs
	 * (bdev2) is shared with other virtual bdevs: bdev5 and bdev6. This
	 * models caching, RAID, partitioning or logical volumes use cases.
	 *
	 * bdev8 is a virtual bdev with multiple base bdevs, but these
	 * base bdevs are themselves virtual bdevs.
	 *
	 *                bdev8
	 *                  |
	 *            +----------+
	 *            |          |
	 *          bdev4      bdev5   bdev6   bdev7
	 *            |          |       |       |
	 *        +---+---+      +---+   +   +---+---+
	 *        |       |           \  |  /         \
	 *      bdev0   bdev1          bdev2         bdev3
	 */

	bdev[0] = allocate_bdev("bdev0");
	rc = spdk_bdev_module_claim_bdev(bdev[0], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[1] = allocate_bdev("bdev1");
	rc = spdk_bdev_module_claim_bdev(bdev[1], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[2] = allocate_bdev("bdev2");
	rc = spdk_bdev_module_claim_bdev(bdev[2], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[3] = allocate_bdev("bdev3");
	rc = spdk_bdev_module_claim_bdev(bdev[3], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[4] = allocate_vbdev("bdev4", bdev[0], bdev[1]);
	rc = spdk_bdev_module_claim_bdev(bdev[4], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[5] = allocate_vbdev("bdev5", bdev[2], NULL);
	rc = spdk_bdev_module_claim_bdev(bdev[5], NULL, &bdev_ut_if);
	CU_ASSERT(rc == 0);

	bdev[6] = allocate_vbdev("bdev6", bdev[2], NULL);

	bdev[7] = allocate_vbdev("bdev7", bdev[2], bdev[3]);

	bdev[8] = allocate_vbdev("bdev8", bdev[4], bdev[5]);

	/* Check tree */
	CU_ASSERT(check_base_and_vbdev(bdev[0], bdev[4]) == true);
	CU_ASSERT(check_base_and_vbdev(bdev[0], bdev[5]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[0], bdev[6]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[0], bdev[7]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[0], bdev[8]) == false);

	CU_ASSERT(check_base_and_vbdev(bdev[1], bdev[4]) == true);
	CU_ASSERT(check_base_and_vbdev(bdev[1], bdev[5]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[1], bdev[6]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[1], bdev[7]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[1], bdev[8]) == false);

	CU_ASSERT(check_base_and_vbdev(bdev[2], bdev[4]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[2], bdev[5]) == true);
	CU_ASSERT(check_base_and_vbdev(bdev[2], bdev[6]) == true);
	CU_ASSERT(check_base_and_vbdev(bdev[2], bdev[7]) == true);
	CU_ASSERT(check_base_and_vbdev(bdev[2], bdev[8]) == false);

	CU_ASSERT(check_base_and_vbdev(bdev[3], bdev[4]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[3], bdev[5]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[3], bdev[6]) == false);
	CU_ASSERT(check_base_and_vbdev(bdev[3], bdev[7]) == true);
	CU_ASSERT(check_base_and_vbdev(bdev[3], bdev[8]) == false);

	/* Open bdev0 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[0], false, NULL, NULL, &desc[0]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[0] != NULL);
	spdk_bdev_close(desc[0]);

	/*
	 * Open bdev1 read/write.  This should fail since bdev1 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[1], true, NULL, NULL, &desc[1]);
	CU_ASSERT(rc == -EPERM);

	/*
	 * Open bdev4 read/write.  This should fail since bdev3 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[4], true, NULL, NULL, &desc[4]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev4 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[4], false, NULL, NULL, &desc[4]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[4] != NULL);
	spdk_bdev_close(desc[4]);

	/*
	 * Open bdev8 read/write.  This should succeed since it is a leaf
	 * bdev.
	 */
	rc = spdk_bdev_open(bdev[8], true, NULL, NULL, &desc[8]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[8] != NULL);
	spdk_bdev_close(desc[8]);

	/*
	 * Open bdev5 read/write.  This should fail since bdev4 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[5], true, NULL, NULL, &desc[5]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev4 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[5], false, NULL, NULL, &desc[5]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[5] != NULL);
	spdk_bdev_close(desc[5]);

	free_vbdev(bdev[8]);

	free_vbdev(bdev[5]);
	free_vbdev(bdev[6]);
	free_vbdev(bdev[7]);

	free_vbdev(bdev[4]);

	free_bdev(bdev[0]);
	free_bdev(bdev[1]);
	free_bdev(bdev[2]);
	free_bdev(bdev[3]);
}

static void
bytes_to_blocks_test(void)
{
	struct spdk_bdev bdev;
	uint64_t offset_blocks, num_blocks;

	memset(&bdev, 0, sizeof(bdev));

	bdev.blocklen = 512;

	/* All parameters valid */
	offset_blocks = 0;
	num_blocks = 0;
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 512, &offset_blocks, 1024, &num_blocks) == 0);
	CU_ASSERT(offset_blocks == 1);
	CU_ASSERT(num_blocks == 2);

	/* Offset not a block multiple */
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 3, &offset_blocks, 512, &num_blocks) != 0);

	/* Length not a block multiple */
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 512, &offset_blocks, 3, &num_blocks) != 0);
}

static void
num_blocks_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_bdev_desc *desc;

	memset(&bdev, 0, sizeof(bdev));
	bdev.name = "num_blocks";
	bdev.fn_table = &fn_table;
	bdev.module = &bdev_ut_if;
	spdk_bdev_register(&bdev);
	spdk_bdev_notify_blockcnt_change(&bdev, 50);

	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 70) == 0);
	/* Shrinking block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 30) == 0);

	/* In case bdev opened */
	spdk_bdev_open(&bdev, false, NULL, NULL, &desc);

	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 80) == 0);
	/* Shrinking block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 20) != 0);

	spdk_bdev_close(desc);
	spdk_bdev_unregister(&bdev, NULL, NULL);
}

static void
io_valid_test(void)
{
	struct spdk_bdev bdev;

	memset(&bdev, 0, sizeof(bdev));

	bdev.blocklen = 512;
	spdk_bdev_notify_blockcnt_change(&bdev, 100);

	/* All parameters valid */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 1, 2) == true);

	/* Last valid block */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 99, 1) == true);

	/* Offset past end of bdev */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 100, 1) == false);

	/* Offset + length past end of bdev */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 99, 2) == false);

	/* Offset near end of uint64_t range (2^64 - 1) */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 18446744073709551615ULL, 1) == false);
}

static void
alias_add_del_test(void)
{
	struct spdk_bdev *bdev[2];
	int rc;

	/* Creating and registering bdevs */
	bdev[0] = allocate_bdev("bdev0");
	SPDK_CU_ASSERT_FATAL(bdev[0] != 0);

	bdev[1] = allocate_bdev("bdev1");
	SPDK_CU_ASSERT_FATAL(bdev[1] != 0);

	/*
	 * Trying adding an alias identical to name.
	 * Alias is identical to name, so it can not be added to aliases list
	 */
	rc = spdk_bdev_alias_add(bdev[0], bdev[0]->name);
	CU_ASSERT(rc == -EEXIST);

	/*
	 * Trying to add empty alias,
	 * this one should fail
	 */
	rc = spdk_bdev_alias_add(bdev[0], NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Trying adding same alias to two different registered bdevs */

	/* Alias is used first time, so this one should pass */
	rc = spdk_bdev_alias_add(bdev[0], "proper alias 0");
	CU_ASSERT(rc == 0);

	/* Alias was added to another bdev, so this one should fail */
	rc = spdk_bdev_alias_add(bdev[1], "proper alias 0");
	CU_ASSERT(rc == -EEXIST);

	/* Alias is used first time, so this one should pass */
	rc = spdk_bdev_alias_add(bdev[1], "proper alias 1");
	CU_ASSERT(rc == 0);

	/* Trying removing an alias from registered bdevs */

	/* Alias is not on a bdev aliases list, so this one should fail */
	rc = spdk_bdev_alias_del(bdev[0], "not existing");
	CU_ASSERT(rc == -ENOENT);

	/* Alias is present on a bdev aliases list, so this one should pass */
	rc = spdk_bdev_alias_del(bdev[0], "proper alias 0");
	CU_ASSERT(rc == 0);

	/* Alias is present on a bdev aliases list, so this one should pass */
	rc = spdk_bdev_alias_del(bdev[1], "proper alias 1");
	CU_ASSERT(rc == 0);

	/* Trying to remove name instead of alias, so this one should fail, name cannot be changed or removed */
	rc = spdk_bdev_alias_del(bdev[0], bdev[0]->name);
	CU_ASSERT(rc != 0);

	/* Unregister and free bdevs */
	spdk_bdev_unregister(bdev[0], NULL, NULL);
	spdk_bdev_unregister(bdev[1], NULL, NULL);

	free(bdev[0]);
	free(bdev[1]);
}

static void
io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_init_cb(void *arg, int rc)
{
	CU_ASSERT(rc == 0);
}

struct bdev_ut_io_wait_entry {
	struct spdk_bdev_io_wait_entry	entry;
	struct spdk_io_channel		*io_ch;
	struct spdk_bdev_desc		*desc;
	bool				submitted;
};

static void
io_wait_cb(void *arg)
{
	struct bdev_ut_io_wait_entry *entry = arg;
	int rc;

	rc = spdk_bdev_read_blocks(entry->desc, entry->io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	entry->submitted = true;
}

static void
bdev_io_wait_test(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_opts bdev_opts = {
		.bdev_io_pool_size = 4,
		.bdev_io_cache_size = 2,
	};
	struct bdev_ut_io_wait_entry io_wait_entry;
	struct bdev_ut_io_wait_entry io_wait_entry2;
	int rc;

	rc = spdk_bdev_set_opts(&bdev_opts);
	CU_ASSERT(rc == 0);
	spdk_bdev_initialize(bdev_init_cb, NULL);

	bdev = allocate_bdev("bdev0");

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc != NULL);
	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);

	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);

	rc = spdk_bdev_read_blocks(desc, io_ch, NULL, 0, 1, io_done, NULL);
	CU_ASSERT(rc == -ENOMEM);

	io_wait_entry.entry.bdev = bdev;
	io_wait_entry.entry.cb_fn = io_wait_cb;
	io_wait_entry.entry.cb_arg = &io_wait_entry;
	io_wait_entry.io_ch = io_ch;
	io_wait_entry.desc = desc;
	io_wait_entry.submitted = false;
	/* Cannot use the same io_wait_entry for two different calls. */
	memcpy(&io_wait_entry2, &io_wait_entry, sizeof(io_wait_entry));
	io_wait_entry2.entry.cb_arg = &io_wait_entry2;

	/* Queue two I/O waits. */
	rc = spdk_bdev_queue_io_wait(bdev, io_ch, &io_wait_entry.entry);
	CU_ASSERT(rc == 0);
	CU_ASSERT(io_wait_entry.submitted == false);
	rc = spdk_bdev_queue_io_wait(bdev, io_ch, &io_wait_entry2.entry);
	CU_ASSERT(rc == 0);
	CU_ASSERT(io_wait_entry2.submitted == false);

	stub_complete_io(1);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);
	CU_ASSERT(io_wait_entry.submitted == true);
	CU_ASSERT(io_wait_entry2.submitted == false);

	stub_complete_io(1);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 4);
	CU_ASSERT(io_wait_entry2.submitted == true);

	stub_complete_io(4);
	CU_ASSERT(g_bdev_ut_channel->outstanding_io_count == 0);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	free_bdev(bdev);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("bdev", null_init, null_clean);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "bytes_to_blocks_test", bytes_to_blocks_test) == NULL ||
		CU_add_test(suite, "num_blocks_test", num_blocks_test) == NULL ||
		CU_add_test(suite, "io_valid", io_valid_test) == NULL ||
		CU_add_test(suite, "open_write", open_write_test) == NULL ||
		CU_add_test(suite, "alias_add_del", alias_add_del_test) == NULL ||
		CU_add_test(suite, "get_device_stat", get_device_stat_test) == NULL ||
		CU_add_test(suite, "bdev_io_wait", bdev_io_wait_test) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	spdk_allocate_thread(_bdev_send_msg, NULL, NULL, NULL, "thread0");
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	spdk_free_thread();
	return num_failures;
}
