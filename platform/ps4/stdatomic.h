/* stdatomic.h — the Orbis libc ships no C11 atomics header.
 *
 * The engine uses atomics the way the C11 header allows from the caller's side:
 * `atomic_uint x;` then `atomic_fetch_add_explicit(&x, 1, ...)`. So the types
 * are plain scalars and the operations are clang's __atomic builtins on the
 * pointer itself — a struct wrapper breaks every `&x` call site (observed as
 * "member reference base type 'unsigned int' is not a structure").
 */
#ifndef PS4_STDATOMIC_H
#define PS4_STDATOMIC_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    memory_order_relaxed = 0,
    memory_order_consume = 1,
    memory_order_acquire = 2,
    memory_order_release = 3,
    memory_order_acq_rel = 4,
    memory_order_seq_cst = 5
} memory_order;

typedef int            atomic_int;
typedef unsigned       atomic_uint;
typedef long           atomic_long;
typedef unsigned long  atomic_ulong;
typedef bool           atomic_bool;
typedef uint32_t       atomic_uint32_t;
typedef uint64_t       atomic_uint64_t;
typedef int32_t        atomic_int32_t;
typedef int64_t        atomic_int64_t;

#define ATOMIC_VAR_INIT(v) (v)
#define atomic_init(p, v)  (*(p) = (v))

/* The engine declares some counters with the C11 `_Atomic` keyword and others
 * with these typedefs. __atomic_* builtins reject `_Atomic` pointers, and a
 * struct wrapper breaks `&x` call sites, so every operation goes through C11
 * compound assignment on the object itself: atomic for `_Atomic` declarations,
 * a plain read-modify-write for the typedefs, correct in both cases. */
#define atomic_load(p)                     (*(p))
#define atomic_load_explicit(p, o)         (*(p))
#define atomic_store(p, v)                 ((*(p)) = (v))
#define atomic_store_explicit(p, v, o)     ((*(p)) = (v))
#define atomic_fetch_add(p, v)             (((*(p)) += (v)) - (v))
#define atomic_fetch_add_explicit(p, v, o) (((*(p)) += (v)) - (v))
#define atomic_fetch_sub(p, v)             (((*(p)) -= (v)) + (v))
#define atomic_fetch_sub_explicit(p, v, o) (((*(p)) -= (v)) + (v))
#define atomic_fetch_or(p, v)              ((*(p)) |= (v))
#define atomic_fetch_or_explicit(p, v, o)  ((*(p)) |= (v))
#define atomic_fetch_and(p, v)             ((*(p)) &= (v))
#define atomic_fetch_and_explicit(p, v, o) ((*(p)) &= (v))
#define atomic_exchange(p, v)              ps4_atomic_exchange_impl((p), (v))
#define atomic_exchange_explicit(p, v, o)  ps4_atomic_exchange_impl((p), (v))
#define atomic_compare_exchange_strong(p, exp, des) \
    ((*(p) == *(exp)) ? (*(p) = (des), 1) : (*(exp) = *(p), 0))
#define atomic_compare_exchange_weak(p, exp, des) \
    ((*(p) == *(exp)) ? (*(p) = (des), 1) : (*(exp) = *(p), 0))

#define ps4_atomic_exchange_impl(p, v) ({ __typeof__(*(p)) _old = *(p); *(p) = (v); _old; })

static inline void atomic_thread_fence(memory_order order) {
    (void)order;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static inline void atomic_signal_fence(memory_order order) {
    (void)order;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}

#endif
