// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2022 Datto Inc.
 */

#include "module_threads.h"
#include "bio_helper.h"
#include "bio_queue.h"
#include "cow_manager.h"
#include "logging.h"
#include "mrf.h"
#include "snap_device.h"
#include "snap_handle.h"
#include "sset_queue.h"
#include "tracer_helper.h"

// this is defined in 3.16 and up
#ifndef MIN_NICE
#define MIN_NICE -20
#endif

int inc_sset_thread(void *data)
{
        int ret, is_failed = 0;
        struct snap_device *dev = data;
        struct sset_queue *sq = &dev->sd_pending_ssets;
        struct sector_set *sset;

        // give this thread the highest priority we are allowed
        set_user_nice(current, MIN_NICE);

        while (!kthread_should_stop() || !sset_queue_empty(sq)) {
                // wait for a sset to process or a kthread_stop call
                wait_event_interruptible(sq->event,
                                         kthread_should_stop() ||
                                                 !sset_queue_empty(sq));

                if (!is_failed && tracer_read_fail_state(dev)) {
                        LOG_DEBUG(
                                "error detected in sset thread, cleaning up cow");
                        is_failed = 1;

                        if (dev->sd_cow)
                                cow_free_members(dev->sd_cow);
                }

                if (sset_queue_empty(sq))
                        continue;

                // safely dequeue a sset
                sset = sset_queue_dequeue(sq);

                // if there has been a problem don't process any more, just free
                // the ones we have
                if (is_failed) {
                        kfree(sset);
                        continue;
                }

                // pass the sset to the handler
                ret = inc_handle_sset(dev, sset);
                if (ret) {
                        LOG_ERROR(ret,
                                  "error handling sector set in kernel thread");
                        tracer_set_fail_state(dev, ret);
                }

                // free the sector set
                kfree(sset);
        }

        return 0;
}

int snap_cow_thread(void *data)
{
        int ret, is_failed = 0;
        struct snap_device *dev = data;
        struct bio_queue *bq = &dev->sd_cow_bios;
        struct bio *bio;

        // give this thread the highest priority we are allowed
        set_user_nice(current, MIN_NICE);

        while (!kthread_should_stop() || !bio_queue_empty(bq) ||
               atomic64_read(&dev->sd_submitted_cnt) !=
                       atomic64_read(&dev->sd_received_cnt)) {
                // wait for a bio to process or a kthread_stop call
                wait_event_interruptible(bq->event,
                                         kthread_should_stop() ||
                                                 !bio_queue_empty(bq));

                if (!is_failed && tracer_read_fail_state(dev)) {
                        LOG_DEBUG(
                                "error detected in cow thread, cleaning up cow");
                        is_failed = 1;

                        if (dev->sd_cow)
                                cow_free_members(dev->sd_cow);
                }

                if (bio_queue_empty(bq))
                        continue;

                // safely dequeue a bio
                bio = bio_queue_dequeue_delay_read(bq);

                // pass bio to handler
                if (!bio_data_dir(bio)) {
                        // if we're in the fail state just send back an IO error
                        // and free the bio
                        if (is_failed) {
                                dattobd_bio_endio(
                                        bio,
                                        -EIO); // end the bio with an IO error
                                continue;
                        }

                        ret = snap_handle_read_bio(dev, bio);
                        if (ret) {
                                LOG_ERROR(
                                        ret,
                                        "error handling read bio in kernel thread");
                                tracer_set_fail_state(dev, ret);
                        }

                        dattobd_bio_endio(bio, (ret) ? -EIO : 0);
                } else {
                        if (is_failed) {
                                bio_free_clone(bio);
                                continue;
                        }

                        ret = snap_handle_write_bio(dev, bio);
                        if (ret) {
                                LOG_ERROR(ret, "error handling write bio in "
                                               "kernel thread");
                                tracer_set_fail_state(dev, ret);
                        }

                        bio_free_clone(bio);
                }
        }

        return 0;
}

int snap_mrf_thread(void *data)
{
        int ret;
        struct snap_device *dev = data;
        struct bio_queue *bq = &dev->sd_orig_bios;
        struct bio *bio;

        MAYBE_UNUSED(ret);

        // give this thread the highest priority we are allowed
        set_user_nice(current, MIN_NICE);

        while (!kthread_should_stop() || !bio_queue_empty(bq)) {
                // wait for a bio to process or a kthread_stop call
                wait_event_interruptible(bq->event,
                                         kthread_should_stop() ||
                                                 !bio_queue_empty(bq));
                if (bio_queue_empty(bq))
                        continue;

                // safely dequeue a bio
                bio = bio_queue_dequeue(bq);

                // submit the original bio to the block IO layer
                dattobd_bio_op_set_flag(bio, DATTOBD_PASSTHROUGH);

                ret = dattobd_call_mrf(dev->sd_orig_mrf,
                                       dattobd_bio_get_queue(bio), bio);
#ifdef HAVE_MAKE_REQUEST_FN_INT
                if (ret)
                        generic_make_request(bio);
#endif
        }

        return 0;
}
