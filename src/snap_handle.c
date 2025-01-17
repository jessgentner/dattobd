// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2022 Datto Inc.
 */

#include "snap_handle.h"

#include "bio_helper.h"
#include "cow_manager.h"
#include "filesystem.h"
#include "logging.h"
#include "snap_device.h"

// macros for snapshot bio modes of operation
#define READ_MODE_COW_FILE 1
#define READ_MODE_BASE_DEVICE 2
#define READ_MODE_MIXED 3

#ifndef READ_SYNC
#define READ_SYNC 0
#endif

static int snap_read_bio_get_mode(const struct snap_device *dev,
                                  struct bio *bio, int *mode)
{
        int ret, start_mode = 0;
        bio_iter_t iter;
        bio_iter_bvec_t bvec;
        unsigned int bytes;
        uint64_t block_mapping, curr_byte,
                curr_end_byte = bio_sector(bio) * SECTOR_SIZE;

        bio_for_each_segment (bvec, bio, iter) {
                // reset the number of bytes we have traversed for this bio_vec
                bytes = 0;

                // while we still have data left to be written into the page
                while (bytes < bio_iter_len(bio, iter)) {
                        // find the start and stop byte for our next write
                        curr_byte = curr_end_byte;
                        curr_end_byte += min(
                                COW_BLOCK_SIZE - (curr_byte % COW_BLOCK_SIZE),
                                ((uint64_t)bio_iter_len(bio, iter)));

                        // check if the mapping exists
                        ret = cow_read_mapping(dev->sd_cow,
                                               curr_byte / COW_BLOCK_SIZE,
                                               &block_mapping);
                        if (ret)
                                goto error;

                        if (!start_mode && block_mapping)
                                start_mode = READ_MODE_COW_FILE;
                        else if (!start_mode && !block_mapping)
                                start_mode = READ_MODE_BASE_DEVICE;
                        else if ((start_mode == READ_MODE_COW_FILE &&
                                  !block_mapping) ||
                                 (start_mode == READ_MODE_BASE_DEVICE &&
                                  block_mapping)) {
                                *mode = READ_MODE_MIXED;
                                return 0;
                        }

                        // increment the number of bytes we have written
                        bytes += curr_end_byte - curr_byte;
                }
        }

        *mode = start_mode;
        return 0;

error:
        LOG_ERROR(ret, "error finding read mode");
        return ret;
}

int snap_handle_read_bio(const struct snap_device *dev, struct bio *bio)
{
        int ret, mode;
        bio_iter_t iter;
        bio_iter_bvec_t bvec;
        void *orig_private;
        bio_end_io_t *orig_end_io;
        char *data;
        sector_t bio_orig_sect, cur_block, cur_sect;
        unsigned int bio_orig_idx, bio_orig_size;
        uint64_t block_mapping, bytes_to_copy, block_off, bvec_off;

        // save the original state of the bio
        orig_private = bio->bi_private;
        orig_end_io = bio->bi_end_io;
        bio_orig_idx = bio_idx(bio);
        bio_orig_size = bio_size(bio);
        bio_orig_sect = bio_sector(bio);

        dattobd_bio_set_dev(bio, dev->sd_base_dev);
        dattobd_set_bio_ops(bio, REQ_OP_READ, READ_SYNC);

        // detect fastpath for bios completely contained within either the cow
        // file or the base device
        ret = snap_read_bio_get_mode(dev, bio, &mode);
        if (ret)
                goto out;

        // submit the bio to the base device and wait for completion
        if (mode != READ_MODE_COW_FILE) {
                ret = dattobd_submit_bio_wait(bio);
                if (ret) {
                        LOG_ERROR(ret,
                                  "error reading from base device for read");
                        goto out;
                }

#ifdef HAVE_BIO_BI_REMAINING
                //#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
                atomic_inc(&bio->bi_remaining);
#endif
        }

        if (mode != READ_MODE_BASE_DEVICE) {
                // reset the bio
                bio_idx(bio) = bio_orig_idx;
                bio_size(bio) = bio_orig_size;
                bio_sector(bio) = bio_orig_sect;
                cur_sect = bio_sector(bio);

                // iterate over all the segments and fill the bio. this more
                // complex than writing since we don't have the block aligned
                // guarantee
                bio_for_each_segment (bvec, bio, iter) {
                        // map the page into kernel space
                        data = kmap(bio_iter_page(bio, iter));

                        cur_block = (cur_sect * SECTOR_SIZE) / COW_BLOCK_SIZE;
                        block_off = (cur_sect * SECTOR_SIZE) % COW_BLOCK_SIZE;
                        bvec_off = bio_iter_offset(bio, iter);

                        while (bvec_off < bio_iter_offset(bio, iter) +
                                                  bio_iter_len(bio, iter)) {
                                bytes_to_copy =
                                        min(bio_iter_offset(bio, iter) +
                                                    bio_iter_len(bio, iter) -
                                                    bvec_off,
                                            COW_BLOCK_SIZE - block_off);
                                // check if the mapping exists
                                ret = cow_read_mapping(dev->sd_cow, cur_block,
                                                       &block_mapping);
                                if (ret) {
                                        kunmap(bio_iter_page(bio, iter));
                                        goto out;
                                }

                                // if the mapping exists, read it into the page,
                                // overwriting the live data
                                if (block_mapping) {
                                        ret = cow_read_data(dev->sd_cow,
                                                            data + bvec_off,
                                                            block_mapping,
                                                            block_off,
                                                            bytes_to_copy);
                                        if (ret) {
                                                kunmap(bio_iter_page(bio,
                                                                     iter));
                                                goto out;
                                        }
                                }

                                cur_sect += bytes_to_copy / SECTOR_SIZE;
                                cur_block = (cur_sect * SECTOR_SIZE) /
                                            COW_BLOCK_SIZE;
                                block_off = (cur_sect * SECTOR_SIZE) %
                                            COW_BLOCK_SIZE;
                                bvec_off += bytes_to_copy;
                        }

                        // unmap the page from kernel space
                        kunmap(bio_iter_page(bio, iter));
                }
        }

out:
        if (ret) {
                LOG_ERROR(ret, "error handling read bio");
                bio_idx(bio) = bio_orig_idx;
                bio_size(bio) = bio_orig_size;
                bio_sector(bio) = bio_orig_sect;
        }

        // revert bio's original data
        bio->bi_private = orig_private;
        bio->bi_end_io = orig_end_io;

        return ret;
}

int snap_handle_write_bio(const struct snap_device *dev, struct bio *bio)
{
        int ret;
        bio_iter_t iter;
        bio_iter_bvec_t bvec;
        char *data;
        sector_t start_block, end_block = SECTOR_TO_BLOCK(bio_sector(bio));

        // iterate through the bio and handle each segment (which is guaranteed
        // to be block aligned)
        bio_for_each_segment (bvec, bio, iter) {
                // find the start and end block
                start_block = end_block;
                end_block = start_block +
                            (bio_iter_len(bio, iter) / COW_BLOCK_SIZE);

                // map the page into kernel space
                data = kmap(bio_iter_page(bio, iter));

                // loop through the blocks in the page
                for (; start_block < end_block; start_block++) {
                        // pas the block to the cow manager to be handled
                        ret = cow_write_current(dev->sd_cow, start_block, data);
                        if (ret) {
                                kunmap(bio_iter_page(bio, iter));
                                goto error;
                        }
                }

                // unmap the page
                kunmap(bio_iter_page(bio, iter));
        }

        return 0;

error:
        LOG_ERROR(ret, "error handling write bio");
        return ret;
}

int inc_handle_sset(const struct snap_device *dev, struct sector_set *sset)
{
        int ret;
        sector_t start_block = SECTOR_TO_BLOCK(sset->sect);
        sector_t end_block = NUM_SEGMENTS(sset->sect + sset->len,
                                          COW_BLOCK_LOG_SIZE - SECTOR_SHIFT);

        for (; start_block < end_block; start_block++) {
                ret = cow_write_filler_mapping(dev->sd_cow, start_block);
                if (ret)
                        goto error;
        }

        return 0;

error:
        LOG_ERROR(ret, "error handling sset");
        return ret;
}
