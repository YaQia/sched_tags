/*
 * test/reader_tokens.c — Userspace-only unit test for the token API.
 *
 * The kernel slot is faked here, so this test runs on any host and checks the
 * pure C logic: nesting, exact keys, atomic set hashes, dep roles, and the
 * compatibility sched_tag_set() wrappers.
 *
 * Build & run (no libsched_tag needed):
 *   cc -std=c11 -I include -I <kernel>/usr/include \
 *      test/reader_tokens.c -o /tmp/test_tokens
 *   /tmp/test_tokens
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../include/sched_tag.h"

/* Provide the library-side symbols locally so we can use a fake slot. */
__thread struct sched_hint *__sched_hint_ptr;
uint64_t __sched_tag_process_salt = 0x123456789ABCDEF0ULL;

struct sched_hint *__sched_hint_register(void)
{
    return __sched_hint_ptr;
}

static struct sched_hint fake_slot;
static int failures;

static void check_u8(const char *what, uint8_t expect, uint8_t actual)
{
    if (expect != actual) {
        printf("FAIL %-42s expected %u, got %u\n", what, expect, actual);
        failures++;
    } else {
        printf("ok   %-42s %u\n", what, actual);
    }
}

static void check_key(const char *what, uint64_t expect, uint64_t actual)
{
    if (expect != actual) {
        printf("FAIL %-42s expected 0x%016llx, got 0x%016llx\n", what,
               (unsigned long long)expect, (unsigned long long)actual);
        failures++;
    } else {
        printf("ok   %-42s 0x%016llx\n", what,
               (unsigned long long)actual);
    }
}

static void reset_slot(void)
{
    memset(&fake_slot, 0, sizeof(fake_slot));
}

int main(void)
{
    int lock_a = 0, lock_b = 0, atomic_a = 0, atomic_b = 0;
    int handoff_q1 = 0, handoff_q2 = 0;
    uint64_t key_a = sched_tag_key_ptr(&lock_a);
    uint64_t key_b = sched_tag_key_ptr(&lock_b);
    uint64_t ka = sched_tag_key_ptr(&atomic_a);
    uint64_t kb = sched_tag_key_ptr(&atomic_b);
    uint64_t q1 = sched_tag_key_ptr(&handoff_q1);
    uint64_t q2 = sched_tag_key_ptr(&handoff_q2);

    __sched_hint_ptr = &fake_slot;

    printf("=== token API unit test ===\n");

    /* --- unshared: overwrite + nested restore --- */
    reset_slot();
    {
        struct sched_tag_token outer = sched_tag_enter_unshared(&lock_a);
        check_u8("unshared outer state", SCHED_UNSHARED_HELD,
                 fake_slot.unshared);
        check_key("unshared outer key", key_a, fake_slot.unshared_magic);

        struct sched_tag_token inner = sched_tag_enter_unshared(&lock_b);
        check_key("unshared inner key", key_b, fake_slot.unshared_magic);

        sched_tag_leave_unshared(&inner);
        check_key("unshared inner leave restores outer", key_a,
                  fake_slot.unshared_magic);

        sched_tag_leave_unshared(&outer);
        check_u8("unshared outer leave state", SCHED_UNSHARED_NONE,
                 fake_slot.unshared);
        check_key("unshared outer leave key", 0, fake_slot.unshared_magic);
    }

    /* --- unshared: static id variant --- */
    reset_slot();
    {
        struct sched_tag_token t = sched_tag_enter_unshared_id(0xDEADBEEFULL);
        check_key("unshared _id key", sched_tag_key_id(0xDEADBEEFULL),
                  fake_slot.unshared_magic);
        sched_tag_leave_unshared(&t);
    }

    /* --- atomic: exact set hash, order independent, restore --- */
    reset_slot();
    {
        struct sched_tag_token t = sched_tag_enter_atomic();
        sched_tag_add_atomic(&atomic_a);
        sched_tag_add_atomic(&atomic_b);
        check_u8("atomic set state", SCHED_ATOMIC_DENSE,
                 fake_slot.atomic_dense);
        check_key("atomic set hash a^b", ka ^ kb, fake_slot.atomic_magic);
        check_u8("atomic exact set probe", 1,
                 sched_tag_test_key(ATOMIC_DENSE, ka ^ kb));
        check_u8("atomic single-member probe is false", 0,
                 sched_tag_test_magic(ATOMIC_DENSE, &atomic_a));
        sched_tag_leave_atomic(&t);
        check_u8("atomic leave state", SCHED_ATOMIC_NONE,
                 fake_slot.atomic_dense);
        check_key("atomic leave magic", 0, fake_slot.atomic_magic);
    }

    /* --- atomic: partial overlap is a different exact key --- */
    reset_slot();
    {
        struct sched_tag_token t = sched_tag_enter_atomic();
        sched_tag_add_atomic(&atomic_a);
        check_key("atomic {a} hash", ka, fake_slot.atomic_magic);
        check_u8("atomic {a} != {a,b}", 1, ka != (ka ^ kb));
        sched_tag_clear_atomic();
        check_u8("atomic clear state", SCHED_ATOMIC_NONE,
                 fake_slot.atomic_dense);
        check_key("atomic clear magic", 0, fake_slot.atomic_magic);
        sched_tag_leave_atomic(&t);
    }

    /* --- dep: handoff key + role, nesting, BOTH, static id --- */
    reset_slot();
    {
        struct sched_tag_token p = sched_tag_enter_dep(&handoff_q1,
                                                       SCHED_DEP_PRODUCER);
        check_u8("dep producer role", SCHED_DEP_PRODUCER, fake_slot.dep_role);
        check_key("dep producer handoff", q1, fake_slot.dep_magic);

        struct sched_tag_token c = sched_tag_enter_dep(&handoff_q2,
                                                       SCHED_DEP_CONSUMER);
        check_u8("dep consumer role", SCHED_DEP_CONSUMER, fake_slot.dep_role);
        check_key("dep consumer handoff", q2, fake_slot.dep_magic);

        sched_tag_leave_dep(&c);
        check_u8("dep leave restores producer role", SCHED_DEP_PRODUCER,
                 fake_slot.dep_role);
        check_key("dep leave restores producer handoff", q1,
                  fake_slot.dep_magic);

        sched_tag_leave_dep(&p);
        check_u8("dep outer leave role", SCHED_DEP_NONE, fake_slot.dep_role);
        check_key("dep outer leave handoff", 0, fake_slot.dep_magic);
    }

    reset_slot();
    {
        struct sched_tag_token t = sched_tag_enter_dep(&handoff_q1,
                                                       SCHED_DEP_BOTH);
        check_u8("dep BOTH role", SCHED_DEP_BOTH, fake_slot.dep_role);
        check_key("dep BOTH handoff", q1, fake_slot.dep_magic);
        sched_tag_leave_dep(&t);
    }

    reset_slot();
    {
        struct sched_tag_token t = sched_tag_enter_dep_id(0xCAFEBABEULL,
                                                          SCHED_DEP_CONSUMER);
        check_key("dep _id handoff", sched_tag_key_id(0xCAFEBABEULL),
                  fake_slot.dep_magic);
        sched_tag_leave_dep(&t);
    }

    reset_slot();
    {
        struct sched_tag_token t = sched_tag_enter_dep(&handoff_q1,
                                                       SCHED_DEP_NONE);
        check_u8("dep NONE role", SCHED_DEP_NONE, fake_slot.dep_role);
        check_key("dep NONE clears handoff", 0, fake_slot.dep_magic);
        sched_tag_leave_dep(&t);
    }

    /* --- compatibility sched_tag_set() --- */
    reset_slot();
    {
        sched_tag_set(UNSHARED, SCHED_UNSHARED_HELD, &lock_a);
        check_u8("compat unshared state", SCHED_UNSHARED_HELD,
                 fake_slot.unshared);
        check_key("compat unshared key", key_a, fake_slot.unshared_magic);
        sched_tag_set(UNSHARED, SCHED_UNSHARED_NONE);
        check_u8("compat unshared clear state", SCHED_UNSHARED_NONE,
                 fake_slot.unshared);
        check_key("compat unshared clear key", 0, fake_slot.unshared_magic);
    }

    reset_slot();
    {
        sched_tag_set(FUNIT_DENSE, SCHED_FUNIT_INT);
        check_u8("compat funit state", SCHED_FUNIT_INT,
                 fake_slot.funit_dense);
        sched_tag_set(FUNIT_DENSE, SCHED_FUNIT_NONE);
        check_u8("compat funit clear", SCHED_FUNIT_NONE,
                 fake_slot.funit_dense);
    }

    reset_slot();
    {
        sched_tag_set(ATOMIC_DENSE, SCHED_ATOMIC_DENSE, &atomic_a);
        check_u8("compat atomic single state", SCHED_ATOMIC_DENSE,
                 fake_slot.atomic_dense);
        check_key("compat atomic single key", ka, fake_slot.atomic_magic);
        sched_tag_set(ATOMIC_DENSE, SCHED_ATOMIC_NONE);
        check_u8("compat atomic clear state", SCHED_ATOMIC_NONE,
                 fake_slot.atomic_dense);
        check_key("compat atomic clear key", 0, fake_slot.atomic_magic);
    }

    reset_slot();
    {
        sched_tag_set(DEP_ROLE, SCHED_DEP_CONSUMER, &handoff_q1);
        check_u8("compat dep state", SCHED_DEP_CONSUMER, fake_slot.dep_role);
        check_key("compat dep key", q1, fake_slot.dep_magic);
        sched_tag_set(DEP_ROLE, SCHED_DEP_NONE);
        check_u8("compat dep clear state", SCHED_DEP_NONE, fake_slot.dep_role);
        check_key("compat dep clear key", 0, fake_slot.dep_magic);
    }

    printf("\n=== %s (%d failure(s)) ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures != 0;
}
