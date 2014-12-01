/**
 * hdr_writer_reader_phaser.h
 * Written by Michael Barker and released to the public domain,
 * as explained at http://creativecommons.org/publicdomain/zero/1.0/
 */

#ifndef HDR_WRITER_READER_PHASER_H
#define HDR_WRITER_READER_PHASER_H 1

#include <stdbool.h>
#include <stdlib.h>

#include <mintomic/mintomic.h>
#include <mintsystem/mutex.h>
#include <mintsystem/timer.h>

MINT_DECL_ALIGNED(struct, 8) hdr_writer_reader_phaser
{
    mint_atomic64_t start_epoch;
    mint_atomic64_t even_end_epoch;
    mint_atomic64_t odd_end_epoch;
    mint_mutex_t* reader_mutex;
};

int64_t _hdr_phaser_get_epoch(mint_atomic64_t* field)
{
    int64_t epoch = mint_load_64_relaxed(field);
    mint_thread_fence_acquire();

    return epoch;    
}

void _hdr_phaser_set_epoch(mint_atomic64_t* field, int64_t val)
{
    mint_thread_fence_release();
    mint_store_64_relaxed(field, val);
    mint_thread_fence_seq_cst();    
}

int64_t _hdr_phaser_reset_epoch(mint_atomic64_t* field, int64_t initial_value)
{
    int64_t current;
    int64_t result;
    do
    {
        current = _hdr_phaser_get_epoch(field);
        result = mint_compare_exchange_strong_64_seq_cst(
            field, current, initial_value);
    }
    while (result != current);

    return result;
}

int hdr_writer_reader_phaser_init(struct hdr_writer_reader_phaser* p)
{
    if (NULL == p)
    {
        return EINVAL;
    }

    p->start_epoch._nonatomic = 0;
    p->even_end_epoch._nonatomic = 0;
    p->odd_end_epoch._nonatomic = INT64_MIN;
    p->reader_mutex = malloc(sizeof(mint_mutex_t));

    if (!p->reader_mutex)
    {
        return ENOMEM;
    }

    int rc = mint_mutex_init(p->reader_mutex);
    if (0 != rc)
    {
        return rc;
    }

    // TODO: Should I fence here.

    return 0;
}

void hdr_writer_reader_phaser_destory(struct hdr_writer_reader_phaser* p)
{
    mint_mutex_destroy(p->reader_mutex);
}

int64_t hdr_phaser_writer_enter(struct hdr_writer_reader_phaser* p)
{
    return mint_fetch_add_64_seq_cst(&p->start_epoch, 1);
}

void hdr_phaser_writer_exit(
    struct hdr_writer_reader_phaser* p, int64_t critical_value_at_enter)
{
    mint_atomic64_t* end_epoch = 
        (critical_value_at_enter < 0) ? &p->odd_end_epoch : &p->even_end_epoch;
    mint_fetch_add_64_seq_cst(end_epoch, 1);
}

void hdr_phaser_reader_lock(struct hdr_writer_reader_phaser* p)
{
    mint_mutex_lock(p->reader_mutex);
}

void hdr_phaser_reader_unlock(struct hdr_writer_reader_phaser* p)
{
    mint_mutex_unlock(p->reader_mutex);
}

void hdr_phaser_flip_phase(
    struct hdr_writer_reader_phaser* p, int64_t sleep_time_ns)
{
    // TODO: is_held_by_current_thread

    int64_t start_epoch = _hdr_phaser_get_epoch(&p->start_epoch);

    bool next_phase_is_even = (start_epoch < 0);

    // Clear currently used phase end epoch.
    int64_t initial_start_value;
    if (next_phase_is_even)
    {
        initial_start_value = 0;
        _hdr_phaser_set_epoch(&p->even_end_epoch, initial_start_value);
    }
    else
    {
        initial_start_value = INT64_MIN;
        _hdr_phaser_set_epoch(&p->odd_end_epoch, initial_start_value);
    }

    // Reset start value, indicating new phase.
    int64_t start_value_at_flip = 
        _hdr_phaser_reset_epoch(&p->start_epoch, initial_start_value);

    bool caught_up = false;
    do
    {
        mint_atomic64_t* end_epoch = 
            next_phase_is_even ? &p->odd_end_epoch : &p->even_end_epoch;

        caught_up = _hdr_phaser_get_epoch(end_epoch) == start_value_at_flip;

        if (!caught_up)
        {
            if (sleep_time_ns == 0)
            {
                mint_yield_hw_thread();
            }
            else
            {
                mint_sleep_millis(sleep_time_ns * 1000000);
            }
        }
    }
    while (!caught_up);
}

#endif