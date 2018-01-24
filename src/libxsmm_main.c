/******************************************************************************
** Copyright (c) 2014-2018, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Hans Pabst, Alexander Heinecke (Intel Corp.)
******************************************************************************/
#include "libxsmm_gemm_diff.h"
#include "libxsmm_trace.h"
#include "libxsmm_trans.h"
#include "libxsmm_gemm.h"
#include "libxsmm_hash.h"
#include "libxsmm_main.h"
#if defined(LIBXSMM_PERF)
# include "libxsmm_perf.h"
#endif
#include "generator_common.h"
#include <libxsmm_intrinsics_x86.h>

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
/* mute warning about target attribute; KNC/native plus JIT is disabled below! */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#if !defined(NDEBUG)
# include <errno.h>
#endif
#if defined(_WIN32)
# include <Windows.h>
#else
# include <sys/mman.h>
# include <unistd.h>
# include <fcntl.h>
#endif
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif

/* LIBXSMM_CAPACITY_REGISTRY is POT */
/*#define LIBXSMM_HASH_MOD(N, NGEN) ((N) % (NGEN))*/
#define LIBXSMM_HASH_MOD(N, NPOT) LIBXSMM_MOD2(N, NPOT)

#if !defined(LIBXSMM_CAPACITY_CACHE)
# define LIBXSMM_CAPACITY_CACHE 4
#endif

#if !defined(LIBXSMM_CODE_MAXSIZE)
# define LIBXSMM_CODE_MAXSIZE 131072
#endif

/* flag fused into the memory address of a code version in case of non-JIT */
#define LIBXSMM_CODE_STATIC (1ULL << (8 * sizeof(void*) - 1))
/* flag fused into the memory address of a code version in case of collision */
#if 0 /* disabled due to no performance advantage */
# define LIBXSMM_HASH_COLLISION (1ULL << (8 * sizeof(void*) - 2))
#endif

#define LIBXSMM_HASH_FUNCTION_CALL(HASH, INDX, DESCRIPTOR) \
  HASH = libxsmm_crc32(DESCRIPTOR, LIBXSMM_GEMM_DESCRIPTOR_SIZE, 25071975/*seed*/); \
  INDX = LIBXSMM_HASH_MOD(HASH, LIBXSMM_CAPACITY_REGISTRY)

LIBXSMM_EXTERN_C typedef struct LIBXSMM_RETARGETABLE internal_statistic_type {
  unsigned int ntry, ncol, njit, nsta;
} internal_statistic_type;

/** Helper macro determining the default prefetch strategy which is used for statically generated kernels. */
#if (0 > LIBXSMM_PREFETCH) /* auto-prefetch (frontend) */ || \
  (defined(_WIN32) || defined(__CYGWIN__)) /* TODO: full support for Windows calling convention */
# define INTERNAL_PREFETCH LIBXSMM_PREFETCH_NONE
#else
# define INTERNAL_PREFETCH LIBXSMM_PREFETCH
#endif

#if defined(LIBXSMM_GEMM_DIFF_SW) && (2 == (LIBXSMM_GEMM_DIFF_SW)) /* most general implementation */
# define INTERNAL_FIND_CODE_CACHE_INDEX(CACHE_HIT, RESULT_INDEX) \
    RESULT_INDEX = ((CACHE_HIT) + ((LIBXSMM_CAPACITY_CACHE) - 1)) % (LIBXSMM_CAPACITY_CACHE)
#else
# define INTERNAL_FIND_CODE_CACHE_INDEX(CACHE_HIT, RESULT_INDEX) \
    assert(/*is pot*/(LIBXSMM_CAPACITY_CACHE) == LIBXSMM_UP2POT(LIBXSMM_CAPACITY_CACHE)); \
    RESULT_INDEX = LIBXSMM_MOD2((CACHE_HIT) + ((LIBXSMM_CAPACITY_CACHE) - 1), LIBXSMM_CAPACITY_CACHE)
#endif

#if defined(_DEBUG)
# define INTERNAL_DISPATCH_DEBUG(RESULT, TYPE, FLAGS, M, N, K, PLDA, PLDB, PLDC, PALPHA, PBETA) \
  if (0 != libxsmm_verbosity && ((INT_MAX) - 1) != libxsmm_verbosity && 0 != (RESULT).pmm) { \
    const libxsmm_blasint internal_dispatch_debug_m_ = M, internal_dispatch_debug_n_ = N, internal_dispatch_debug_k_ = K; \
    const libxsmm_blasint internal_dispatch_debug_lda_ = (0 == (PLDA) ? M : *(PLDA)); \
    const libxsmm_blasint internal_dispatch_debug_ldb_ = (0 == (PLDB) ? K : *(PLDB)); \
    const libxsmm_blasint internal_dispatch_debug_ldc_ = (0 == (PLDC) ? M : *(PLDC)); \
    LIBXSMM_FLOCK(stdout); \
    fprintf(stdout, "LIBXSMM: "); \
    LIBXSMM_GEMM_PRINT(stdout, LIBXSMM_GEMM_PRECISION(TYPE), FLAGS, \
      &internal_dispatch_debug_m_, &internal_dispatch_debug_n_, &internal_dispatch_debug_k_, \
      PALPHA, 0/*a*/, &internal_dispatch_debug_lda_, 0/*b*/, &internal_dispatch_debug_ldb_, PBETA, 0/*c*/, &internal_dispatch_debug_ldc_); \
    fprintf(stdout, " = %p\n", (RESULT).pmm); \
    LIBXSMM_FUNLOCK(stdout); \
  }
#else
# define INTERNAL_DISPATCH_DEBUG(RESULT, TYPE, FLAGS, M, N, K, PLDA, PLDB, PLDC, PALPHA, PBETA)
#endif

#define INTERNAL_DISPATCH(TYPE, DESC, PFLAGS, M, N, K, PLDA, PLDB, PLDC, PALPHA, PBETA, PREFETCH) { \
  const libxsmm_blasint ilda = (0 == (PLDA) ? m : *(PLDA)), ildb = (0 == (PLDB) ? k : *(PLDB)), ildc = (0 == (PLDC) ? m : *(PLDC)); \
  const int internal_prefetch = (0 == (PREFETCH) ? libxsmm_gemm_auto_prefetch : *(PREFETCH)); \
  const int iflags = (0 == (PFLAGS) ? LIBXSMM_FLAGS : *(PFLAGS)); \
  libxsmm_code_pointer internal_dispatch_result_; \
  libxsmm_gemm_descriptor DESC; \
  if (EXIT_SUCCESS == LIBXSMM_CONCATENATE(LIBXSMM_CONCATENATE(libxsmm_, LIBXSMM_TPREFIX_NAME(TYPE)), gemm_descriptor_init)( \
    &(DESC), M, N, K, ilda, ildb, ildc, \
    0 != (PALPHA) ? *((const TYPE*)(PALPHA)) : (LIBXSMM_ALPHA), \
    0 != (PBETA) ? *((const TYPE*)(PBETA)) : (LIBXSMM_BETA), \
    iflags, internal_prefetch)) \
  { \
    internal_dispatch_result_ = internal_find_code(&(DESC)); \
  } \
  else { /* unsupported */ \
    libxsmm_update_mmstatistic(LIBXSMM_GEMM_PRECISION(TYPE), M, N, K, 1/*try*/, 0); \
    internal_dispatch_result_.pmm = 0; \
  } \
  INTERNAL_DISPATCH_DEBUG(internal_dispatch_result_, TYPE, \
    0 == (PFLAGS) ? LIBXSMM_FLAGS : *(PFLAGS), \
    M, N, K, PLDA, PLDB, PLDC, PALPHA, PBETA); \
  return internal_dispatch_result_.xgemm.LIBXSMM_TPREFIX(TYPE, mm); \
}

#if !defined(LIBXSMM_NO_SYNC)
# if !defined(INTERNAL_REGLOCK_MAXN)
#   if 0 /* RW-lock */
#   define INTERNAL_REGLOCK_MAXN 0
#   else
#   define INTERNAL_REGLOCK_MAXN 256
#   endif
# endif
# if (0 < INTERNAL_REGLOCK_MAXN)
#   if LIBXSMM_LOCK_TYPE_ISPOD(LIBXSMM_REGLOCK)
LIBXSMM_EXTERN_C typedef union LIBXSMM_RETARGETABLE internal_reglocktype {
  char pad[LIBXSMM_CACHELINE];
  LIBXSMM_LOCK_TYPE(LIBXSMM_REGLOCK) state;
} internal_reglocktype;
#   else
LIBXSMM_EXTERN_C typedef union LIBXSMM_RETARGETABLE internal_reglocktype {
  LIBXSMM_LOCK_TYPE(LIBXSMM_REGLOCK) state;
} internal_reglocktype;
#   endif
LIBXSMM_API_VARIABLE(internal_reglocktype internal_reglock[INTERNAL_REGLOCK_MAXN]);
# else /* RW-lock */
LIBXSMM_API_VARIABLE(LIBXSMM_LOCK_ATTR_TYPE(LIBXSMM_LOCK_RWLOCK) internal_reglock_attr);
LIBXSMM_API_VARIABLE(LIBXSMM_LOCK_TYPE(LIBXSMM_LOCK_RWLOCK) internal_reglock);
# endif
#endif

/** Determines the try-lock property (1<N: disabled, N=1: enabled [N=0: disabled in case of RW-lock]). */
LIBXSMM_API_VARIABLE(int internal_reglock_count);
LIBXSMM_API_VARIABLE(size_t internal_registry_nbytes);
LIBXSMM_API_VARIABLE(libxsmm_kernel_info* internal_registry_keys);
LIBXSMM_API_VARIABLE(libxsmm_code_pointer* internal_registry);
LIBXSMM_API_VARIABLE(internal_statistic_type internal_statistic[2/*DP/SP*/][4/*sml/med/big/xxx*/]);
LIBXSMM_API_VARIABLE(unsigned int internal_statistic_sml);
LIBXSMM_API_VARIABLE(unsigned int internal_statistic_med);
LIBXSMM_API_VARIABLE(unsigned int internal_statistic_mnk);
LIBXSMM_API_VARIABLE(unsigned int internal_statistic_num_mcopy);
LIBXSMM_API_VARIABLE(unsigned int internal_statistic_num_tcopy);
LIBXSMM_API_VARIABLE(unsigned int internal_teardown);
LIBXSMM_API_VARIABLE(int internal_dispatch_trylock_locked);
LIBXSMM_API_VARIABLE(int internal_gemm_auto_prefetch_locked);


#if defined(LIBXSMM_NO_SYNC)
# define INTERNAL_FIND_CODE_LOCK(LOCKINDEX, INDEX, DIFF, CODE)
# define INTERNAL_FIND_CODE_UNLOCK(LOCKINDEX)
#elif (0 < INTERNAL_REGLOCK_MAXN)
# define INTERNAL_FIND_CODE_LOCK(LOCKINDEX, INDEX, DIFF, CODE) { \
  const unsigned int LOCKINDEX = LIBXSMM_MOD2(INDEX, internal_reglock_count); \
  if (LIBXSMM_LOCK_ACQUIRED(LIBXSMM_REGLOCK) != LIBXSMM_LOCK_TRYLOCK(LIBXSMM_REGLOCK, &internal_reglock[LOCKINDEX].state)) { \
    if (1 != internal_reglock_count && /* (re-)try and get (meanwhile) generated code */ \
        0 != internal_registry) /* ensure engine is not shut down */ \
    { \
      continue; \
    } \
    else { /* exit dispatch and let client fall back */ \
      DIFF = 0; CODE = 0; \
      break; \
    } \
  }
# define INTERNAL_FIND_CODE_UNLOCK(LOCKINDEX) LIBXSMM_LOCK_RELEASE(LIBXSMM_REGLOCK, &internal_reglock[LOCKINDEX].state); }
#else /* RW-lock */
# define INTERNAL_FIND_CODE_LOCK(LOCKINDEX, INDEX, DIFF, CODE) { \
  if (LIBXSMM_LOCK_ACQUIRED(LIBXSMM_LOCK_RWLOCK) != LIBXSMM_LOCK_TRYLOCK(LIBXSMM_LOCK_RWLOCK, &internal_reglock)) { \
    if (1 != internal_reglock_count && /* (re-)try and get (meanwhile) generated code */ \
        0 != internal_registry) /* ensure engine is not shut down */ \
    { \
      continue; \
    } \
    else { /* exit dispatch and let client fall back */ \
      DIFF = 0; CODE = 0; \
      break; \
    } \
  }
# define INTERNAL_FIND_CODE_UNLOCK(LOCKINDEX) LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK_RWLOCK, &internal_reglock); }
#endif


LIBXSMM_API_DEFINITION unsigned int libxsmm_update_mmstatistic(libxsmm_gemm_precision precision,
  libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k, unsigned int ntry, unsigned int ncol)
{
  const unsigned long long kernel_size = LIBXSMM_MNK_SIZE(m, n, k);
  const int index = (LIBXSMM_GEMM_PRECISION_F64 == precision ? 0 : 1);
  int bucket = 3/*huge*/;

  if (LIBXSMM_MNK_SIZE(internal_statistic_sml, internal_statistic_sml, internal_statistic_sml) >= kernel_size) {
    bucket = 0;
  }
  else if (LIBXSMM_MNK_SIZE(internal_statistic_med, internal_statistic_med, internal_statistic_med) >= kernel_size) {
    bucket = 1;
  }
  else if (LIBXSMM_MNK_SIZE(internal_statistic_mnk, internal_statistic_mnk, internal_statistic_mnk) >= kernel_size) {
    bucket = 2;
  }

  LIBXSMM_ATOMIC_ADD_FETCH(&internal_statistic[index][bucket].ncol, ncol, LIBXSMM_ATOMIC_RELAXED);
  return LIBXSMM_ATOMIC_ADD_FETCH(&internal_statistic[index][bucket].ntry, ntry, LIBXSMM_ATOMIC_RELAXED);
}


LIBXSMM_API_INLINE unsigned int internal_update_mmstatistic(const libxsmm_gemm_descriptor* desc,
  unsigned int ntry, unsigned int ncol)
{
  assert(0 != desc && LIBXSMM_KERNEL_KIND_MATMUL == desc->iflags);
  return libxsmm_update_mmstatistic((libxsmm_gemm_precision)desc->datatype, desc->m, desc->n, desc->k, ntry, ncol);
}


LIBXSMM_API_INLINE const char* internal_get_target_arch(int id);
LIBXSMM_API_INLINE const char* internal_get_target_arch(int id)
{
  const char* target_arch = 0;
  switch (id) {
    case LIBXSMM_X86_AVX512_ICL: {
      target_arch = "icl";
    } break;
    case LIBXSMM_X86_AVX512_CORE: {
      target_arch = "skx";
    } break;
    case LIBXSMM_X86_AVX512_KNM: {
      target_arch = "knm";
    } break;
    case LIBXSMM_X86_AVX512_MIC: {
      target_arch = "knl";
    } break;
    case LIBXSMM_X86_AVX512: {
      target_arch = "avx3";
    } break;
    case LIBXSMM_X86_AVX2: {
      target_arch = "hsw";
    } break;
    case LIBXSMM_X86_AVX: {
      target_arch = "snb";
    } break;
    case LIBXSMM_X86_SSE4: {
      target_arch = "wsm";
    } break;
    case LIBXSMM_X86_SSE3: {
      target_arch = "sse3";
    } break;
    case LIBXSMM_TARGET_ARCH_GENERIC: {
      target_arch = "generic";
    } break;
    default: if (LIBXSMM_X86_GENERIC <= id) {
      target_arch = "x86";
    }
    else {
      target_arch = "unknown";
    }
  }

  assert(0 != target_arch);
  return target_arch;
}


LIBXSMM_API_INLINE unsigned int internal_print_number(unsigned int n, char default_unit, char* unit)
{
  unsigned int number = n;
  assert(0 != unit);
  *unit = default_unit;
  if ((1000000) <= n) {
    number = (n + 500000) / 1000000;
    *unit = 'm';
  }
  else if (9999 < n) {
    number = (n + 500) / 1000;
    *unit = 'k';
  }
  return number;
}


LIBXSMM_API_INLINE unsigned int internal_print_statistic(FILE* ostream,
  const char* target_arch, int precision, unsigned int linebreaks, unsigned int indent)
{
  const internal_statistic_type statistic_sml = internal_statistic[precision][0/*SML*/];
  const internal_statistic_type statistic_med = internal_statistic[precision][1/*MED*/];
  const internal_statistic_type statistic_big = internal_statistic[precision][2/*BIG*/];
  const internal_statistic_type statistic_xxx = internal_statistic[precision][3/*XXX*/];
  int printed = 0;
  assert(0 != ostream && (0 <= precision && precision < 2));

  if (/* omit to print anything if it is superfluous */
    0 != statistic_sml.ntry || 0 != statistic_sml.njit || 0 != statistic_sml.nsta || 0 != statistic_sml.ncol ||
    0 != statistic_med.ntry || 0 != statistic_med.njit || 0 != statistic_med.nsta || 0 != statistic_med.ncol ||
    0 != statistic_big.ntry || 0 != statistic_big.njit || 0 != statistic_big.nsta || 0 != statistic_big.ncol ||
    0 != statistic_xxx.ntry || 0 != statistic_xxx.njit || 0 != statistic_xxx.nsta || 0 != statistic_xxx.ncol)
  {
    char title[256], range[256], unit[4];
    unsigned int counter[4];
    {
      unsigned int n;
      if (0 != target_arch && 0 != *target_arch) {
        assert(strlen(target_arch) < sizeof(title));
        for (n = 0; 0 != target_arch[n] /*avoid code-gen. issue with some clang versions: && n < sizeof(title)*/; ++n) {
          const char c = target_arch[n];
          title[n] = (char)(('a' <= c && c <= 'z') ? (c - 32) : c); /* toupper */
        }
        LIBXSMM_SNPRINTF(title + n, sizeof(title) - n, "/%s", 0 == precision ? "DP" : "SP");
      }
      else {
        LIBXSMM_SNPRINTF(title, sizeof(title), "%s", 0 == precision ? "DP" : "SP");
      }
      for (n = 0; n < linebreaks; ++n) fprintf(ostream, "\n");
    }
    fprintf(ostream, "%*s%-8s %6s %6s %6s %6s\n", (int)indent, "", title, "TRY", "JIT", "STA", "COL");
    LIBXSMM_SNPRINTF(range, sizeof(range), "%u..%u", 0u, internal_statistic_sml);
    counter[0] = internal_print_number(statistic_sml.ntry, ' ', unit + 0);
    counter[1] = internal_print_number(statistic_sml.njit, ' ', unit + 1);
    counter[2] = internal_print_number(statistic_sml.nsta, ' ', unit + 2);
    counter[3] = internal_print_number(statistic_sml.ncol, ' ', unit + 3);
    fprintf(ostream, "%*s%8s %6u%c %5u%c %5u%c %5u%c\n", (int)indent, "", range,
      counter[0], unit[0], counter[1], unit[1], counter[2], unit[2], counter[3], unit[3]);
    LIBXSMM_SNPRINTF(range, sizeof(range), "%u..%u", internal_statistic_sml + 1u, internal_statistic_med);
    counter[0] = internal_print_number(statistic_med.ntry, ' ', unit + 0);
    counter[1] = internal_print_number(statistic_med.njit, ' ', unit + 1);
    counter[2] = internal_print_number(statistic_med.nsta, ' ', unit + 2);
    counter[3] = internal_print_number(statistic_med.ncol, ' ', unit + 3);
    fprintf(ostream, "%*s%8s %6u%c %5u%c %5u%c %5u%c\n", (int)indent, "", range,
      counter[0], unit[0], counter[1], unit[1], counter[2], unit[2], counter[3], unit[3]);
    LIBXSMM_SNPRINTF(range, sizeof(range), "%u..%u", internal_statistic_med + 1u, internal_statistic_mnk);
    counter[0] = internal_print_number(statistic_big.ntry, ' ', unit + 0);
    counter[1] = internal_print_number(statistic_big.njit, ' ', unit + 1);
    counter[2] = internal_print_number(statistic_big.nsta, ' ', unit + 2);
    counter[3] = internal_print_number(statistic_big.ncol, ' ', unit + 3);
    fprintf(ostream, "%*s%8s %6u%c %5u%c %5u%c %5u%c\n", (int)indent, "", range,
      counter[0], unit[0], counter[1], unit[1], counter[2], unit[2], counter[3], unit[3]);
    if (0 != statistic_xxx.ntry || 0 != statistic_xxx.njit || 0 != statistic_xxx.nsta || 0 != statistic_xxx.ncol) {
      LIBXSMM_SNPRINTF(range, sizeof(range), "> %u", internal_statistic_mnk);
      counter[0] = internal_print_number(statistic_xxx.ntry, ' ', unit + 0);
      counter[1] = internal_print_number(statistic_xxx.njit, ' ', unit + 1);
      counter[2] = internal_print_number(statistic_xxx.nsta, ' ', unit + 2);
      counter[3] = internal_print_number(statistic_xxx.ncol, ' ', unit + 3);
      fprintf(ostream, "%*s%8s %6u%c %5u%c %5u%c %5u%c\n", (int)indent, "", range,
        counter[0], unit[0], counter[1], unit[1], counter[2], unit[2], counter[3], unit[3]);
    }
    printed = 1;
  }

  return printed;
}


LIBXSMM_API_INLINE unsigned int internal_statistic_ntry(int precision)
{
  return internal_statistic[precision][0/*SML*/].ntry + internal_statistic[precision][1/*MED*/].ntry
       + internal_statistic[precision][2/*BIG*/].ntry + internal_statistic[precision][3/*XXX*/].ntry;
}


LIBXSMM_API void internal_register_static_code(const libxsmm_gemm_descriptor*,
  unsigned int, unsigned int, libxsmm_xmmfunction, libxsmm_code_pointer*);
LIBXSMM_API_DEFINITION void internal_register_static_code(const libxsmm_gemm_descriptor* desc,
  unsigned int index, unsigned int hash, libxsmm_xmmfunction src, libxsmm_code_pointer* registry)
{
  libxsmm_kernel_info* dst_key = internal_registry_keys + index;
  libxsmm_code_pointer* dst_entry = registry + index;
#if !defined(NDEBUG)
  libxsmm_code_pointer code; code.xgemm = src;
  assert(0 != desc && 0 != code.ptr_const && 0 != dst_key && 0 != registry);
  assert(0 == (LIBXSMM_CODE_STATIC & code.uval));
#endif

  if (0 != dst_entry->ptr_const) { /* collision? */
    /* start at a re-hashed index position */
    const unsigned int start = LIBXSMM_HASH_MOD(libxsmm_crc32_u32(151981/*seed*/, hash), LIBXSMM_CAPACITY_REGISTRY);
    unsigned int i0, i, next;
#if defined(LIBXSMM_HASH_COLLISION)
    /* mark current entry as a collision (this might be already the case) */
    dst_entry->uval |= LIBXSMM_HASH_COLLISION;
#endif
    /* start linearly searching for an available slot */
    for (i = (start != index) ? start : LIBXSMM_HASH_MOD(start + 1, LIBXSMM_CAPACITY_REGISTRY), i0 = i, next = LIBXSMM_HASH_MOD(i + 1, LIBXSMM_CAPACITY_REGISTRY);
      0 != registry[i].ptr_const && next != i0; i = next, next = LIBXSMM_HASH_MOD(i + 1, LIBXSMM_CAPACITY_REGISTRY));

    /* calculate destinations */
    dst_key = internal_registry_keys + i;
    dst_entry = registry + i;

    internal_update_mmstatistic(desc, 0, 1/*collision*/);
  }

  if (0 == dst_entry->ptr_const) { /* registry not (yet) exhausted */
    dst_key->xgemm = *desc;
    dst_entry->xgemm = src;
    /* mark current entry as static code (non-JIT) */
    dst_entry->uval |= LIBXSMM_CODE_STATIC;
  }

  internal_update_mmstatistic(desc, 1/*try*/, 0);
}


LIBXSMM_API_INLINE void internal_finalize(void)
{
  libxsmm_finalize();
  if (0 != libxsmm_verbosity) { /* print statistic on termination */
    fflush(stdout); /* synchronize with standard output */
    {
      const char *const env_target_hidden = getenv("LIBXSMM_TARGET_HIDDEN");
      const char *const target_arch = (0 == env_target_hidden || 0 == atoi(env_target_hidden))
        ? internal_get_target_arch(libxsmm_target_archid)
        : 0/*hidden*/;
      const double regsize = 1.0 * internal_registry_nbytes / (1 << 20);
      libxsmm_scratch_info scratch_info;
      unsigned int linebreak;

      if (1 < libxsmm_verbosity || 0 > libxsmm_verbosity) {
        fprintf(stderr, "\nLIBXSMM_VERSION=%s-%s", LIBXSMM_BRANCH, LIBXSMM_VERSION);
      }
      linebreak = (0 == internal_print_statistic(stderr, target_arch, 1/*SP*/, 1, 0)) ? 1 : 0;
      if (0 == internal_print_statistic(stderr, target_arch, 0/*DP*/, linebreak, 0) && 0 != linebreak && 0 != target_arch) {
        fprintf(stderr, "\nLIBXSMM_TARGET=%s", target_arch);
      }
      fprintf(stderr, "\nRegistry: %.f MB", regsize);
      if (1 < libxsmm_verbosity || 0 > libxsmm_verbosity) {
        size_t ngemms = 0;
        int i; for (i = 0; i < 4; ++i) {
          ngemms += internal_statistic[0/*DP*/][i].nsta + internal_statistic[1/*SP*/][i].nsta;
          ngemms += internal_statistic[0/*DP*/][i].njit + internal_statistic[1/*SP*/][i].njit;
        }
        fprintf(stderr, " (gemm=%lu mcopy=%u tcopy=%u)", (unsigned long int)ngemms,
          internal_statistic_num_mcopy, internal_statistic_num_tcopy);
      }
      if (EXIT_SUCCESS == libxsmm_get_scratch_info(&scratch_info) && 0 < scratch_info.size) {
        fprintf(stderr, "\nScratch: %.f MB", 1.0 * scratch_info.size / (1 << 20));
        if (1 < libxsmm_verbosity || 0 > libxsmm_verbosity) {
#if !defined(LIBXSMM_NO_SYNC)
          if (1 < libxsmm_threads_count) {
            fprintf(stderr, " (mallocs=%lu, pools=%u, threads=%u)\n",
              (unsigned long int)scratch_info.nmallocs,
              scratch_info.npools, libxsmm_threads_count);
          }
          else
#endif
          {
            fprintf(stderr, " (mallocs=%lu, pools=%u)\n",
              (unsigned long int)scratch_info.nmallocs,
              scratch_info.npools);
          }
        }
        else {
          fprintf(stderr, "\n");
        }
      }
      else {
        fprintf(stderr, "\n");
      }
    }
  }

  /* release scratch memory pool */
  libxsmm_release_scratch();

#if !defined(LIBXSMM_NO_SYNC)
  { /* release locks */
# if (0 < INTERNAL_REGLOCK_MAXN)
    int i; for (i = 0; i < internal_reglock_count; ++i) LIBXSMM_LOCK_DESTROY(LIBXSMM_REGLOCK, &internal_reglock[i].state);
# else
    LIBXSMM_LOCK_DESTROY(LIBXSMM_LOCK_RWLOCK, &internal_reglock);
    LIBXSMM_LOCK_ATTR_DESTROY(LIBXSMM_LOCK_RWLOCK, &internal_reglock_attr);
# endif
    LIBXSMM_LOCK_DESTROY(LIBXSMM_LOCK, &libxsmm_lock_global);
    LIBXSMM_LOCK_ATTR_DESTROY(LIBXSMM_LOCK, &libxsmm_lock_attr_default);
  }
#endif
}


LIBXSMM_API_INLINE void internal_init(void)
{
#if defined(LIBXSMM_TRACE)
  int filter_threadid = 0, filter_mindepth = -1, filter_maxnsyms = 0, init_code = EXIT_SUCCESS;
#endif
  int i;
  const libxsmm_malloc_function null_malloc_fn = { 0 };
  const libxsmm_free_function null_free_fn = { 0 };
#if !defined(LIBXSMM_NO_SYNC) /* setup the locks in a thread-safe fashion */
  LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK, &libxsmm_lock_global);
# if (0 < INTERNAL_REGLOCK_MAXN)
  for (i = 0; i < internal_reglock_count; ++i) LIBXSMM_LOCK_ACQUIRE(LIBXSMM_REGLOCK, &internal_reglock[i].state);
# else
  LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK_RWLOCK, &internal_reglock);
# endif
#endif
  if (0 == internal_registry) { /* double-check after acquiring the lock(s) */
    assert(0 == internal_registry_keys); /* should never happen */
    libxsmm_xset_default_allocator(0/*lock*/, 0/*context*/, null_malloc_fn, null_free_fn);
    libxsmm_xset_scratch_allocator(0/*lock*/, 0/*context*/, null_malloc_fn, null_free_fn);
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
    { const char *const env = getenv("LIBXSMM_SCRATCH_POOLS");
      if (0 == env || 0 == *env) {
        libxsmm_scratch_pools = LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS;
      }
      else {
        libxsmm_scratch_pools = LIBXSMM_CLMP(atoi(env), 0, LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS);
        /*libxsmm_scratch_pools_locked = 1;*/
      }
      assert(libxsmm_scratch_pools <= LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS);
    }
    { const char *const env = getenv("LIBXSMM_SCRATCH_LIMIT");
      if (0 == env || 0 == *env) {
        /*const*/ unsigned long long limit = LIBXSMM_MALLOC_SCRATCH_LIMIT;
        libxsmm_scratch_limit = (size_t)limit;
      }
      else {
        size_t u = strlen(env) - 1; /* 0 < strlen(env) */
        const char *const unit = "kmgKMG", *const hit = strchr(unit, env[u]);
        libxsmm_scratch_limit = (size_t)strtoul(env, 0, 10);
        u = (0 != hit ? ((hit - unit) % 3) : 3);
        if (u < 3) {
          libxsmm_scratch_limit <<= (u + 1) * 10;
        }
        /*libxsmm_scratch_limit_locked = 1;*/
      }
    }
    { const char *const env = getenv("LIBXSMM_SCRATCH_SCALE");
      if (0 == env || 0 == *env) {
        libxsmm_scratch_scale = LIBXSMM_MALLOC_SCRATCH_SCALE;
      }
      else {
        libxsmm_scratch_scale = LIBXSMM_CLMP(atof(env), 1.1, 3.0);
        /*libxsmm_scratch_scale_locked = 1;*/
      }
      assert(1 <= libxsmm_scratch_scale);
    }
#endif /*defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))*/
    libxsmm_set_target_arch(getenv("LIBXSMM_TARGET")); /* set libxsmm_target_archid */
    { const char *const env = getenv("LIBXSMM_SYNC");
      libxsmm_nosync = (0 == env || 0 == *env) ? 0/*default*/ : atoi(env);
    }
    /* clear internal counters/statistic */
    for (i = 0; i < 4/*sml/med/big/xxx*/; ++i) {
      memset(&internal_statistic[0/*DP*/][i], 0, sizeof(internal_statistic_type));
      memset(&internal_statistic[1/*SP*/][i], 0, sizeof(internal_statistic_type));
    }
    libxsmm_nt = 2;
#if !defined(__MIC__) && (LIBXSMM_X86_AVX512_MIC != LIBXSMM_STATIC_TARGET_ARCH)
    if (LIBXSMM_X86_AVX512_MIC == libxsmm_target_archid)
#endif
    {
      libxsmm_nt = 4;
    }
    {
      const char *const env = getenv("LIBXSMM_VERBOSE");
      if (0 != env && 0 != *env) {
        libxsmm_verbosity = atoi(env);
      }
#if !defined(NDEBUG)
      else {
        libxsmm_verbosity = INT_MAX - 1; /* quiet -> verbose */
      }
#endif
    }
    internal_statistic_mnk = libxsmm_cbrt_u64(LIBXSMM_MAX_MNK);
    internal_statistic_sml = 13;
    internal_statistic_med = 23;
#if defined(LIBXSMM_TRACE)
    { const char *const env = getenv("LIBXSMM_TRACE");
      if (0 != env && 0 != *env) {
        char buffer[32] = { 0 };
        if (1 == sscanf(env, "%32[^,],", buffer)) {
          init_code = (0 <= sscanf(buffer, "%i", &filter_threadid) ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        if (1 == sscanf(env, "%*[^,],%32[^,],", buffer)) {
          init_code = (0 <= sscanf(buffer, "%i", &filter_mindepth) ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        if (1 == sscanf(env, "%*[^,],%*[^,],%32s", buffer)) {
          init_code = (0 <= sscanf(buffer, "%i", &filter_maxnsyms) ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        else {
          filter_maxnsyms = -1; /* all */
        }
      }
    }
    if (EXIT_SUCCESS == init_code) {
      init_code = libxsmm_trace_init(filter_threadid - 1, filter_mindepth, filter_maxnsyms);
      if (EXIT_SUCCESS == init_code)
#endif
      {
        libxsmm_code_pointer *const new_registry = (libxsmm_code_pointer*)malloc((LIBXSMM_CAPACITY_REGISTRY) * sizeof(libxsmm_code_pointer));
        internal_registry_keys = (libxsmm_kernel_info*)malloc((LIBXSMM_CAPACITY_REGISTRY) * sizeof(libxsmm_kernel_info));
        if (0 != new_registry && 0 != internal_registry_keys) {
          const char *const env = getenv("LIBXSMM_GEMM_PREFETCH");
          libxsmm_gemm_diff_init(libxsmm_target_archid);
          libxsmm_trans_init(libxsmm_target_archid);
          libxsmm_hash_init(libxsmm_target_archid);
          libxsmm_dnn_init(libxsmm_target_archid);
#if defined(LIBXSMM_PERF)
          libxsmm_perf_init();
#endif
          for (i = 0; i < (LIBXSMM_CAPACITY_REGISTRY); ++i) new_registry[i].pmm = 0;
          /* omit registering code if JIT is enabled and if an ISA extension is found
           * which is beyond the static code path used to compile the library
           */
#if defined(LIBXSMM_BUILD)
# if (0 != LIBXSMM_JIT) && !defined(__MIC__)
          /* check if target arch. permits execution (arch. may be overridden) */
          if (LIBXSMM_STATIC_TARGET_ARCH <= libxsmm_target_archid &&
            (LIBXSMM_X86_AVX > libxsmm_target_archid /* JIT code gen. is not available */
              /* condition allows to avoid JIT (if static code is good enough) */
              || LIBXSMM_STATIC_TARGET_ARCH == libxsmm_target_archid))
# endif
          { /* opening a scope for eventually declaring variables */
            /* setup the dispatch table for the statically generated code */
#           include <libxsmm_dispatch.h>
          }
#endif
#if defined(_WIN32) || defined(__CYGWIN__) /* TODO: full support for Windows calling convention */
          libxsmm_gemm_auto_prefetch_default = INTERNAL_PREFETCH;
#else
          libxsmm_gemm_auto_prefetch_default = (0 == internal_statistic_ntry(0/*DP*/) && 0 == internal_statistic_ntry(1/*SP*/))
            /* avoid special prefetch if static code is present, since such code uses INTERNAL_PREFETCH */
            ? (((LIBXSMM_X86_AVX512 >= libxsmm_target_archid || LIBXSMM_X86_AVX512_CORE <= libxsmm_target_archid))
              ? LIBXSMM_PREFETCH_AL2BL2_VIA_C : LIBXSMM_PREFETCH_BL2_VIA_C)
            : INTERNAL_PREFETCH;
#endif
          libxsmm_gemm_auto_prefetch = INTERNAL_PREFETCH;
          if (0 != env && 0 != *env) { /* user input beyond auto-prefetch is always considered */
            const int uid = atoi(env);
            if (0 <= uid) {
              libxsmm_gemm_auto_prefetch_default = libxsmm_gemm_uid2prefetch(uid);
              libxsmm_gemm_auto_prefetch = libxsmm_gemm_auto_prefetch_default;
              internal_gemm_auto_prefetch_locked = 1;
            }
          }
          libxsmm_gemm_init(libxsmm_target_archid);
          if (0 == internal_teardown) {
            atexit(internal_finalize);
          }
          {
            void *const pv_registry = &internal_registry;
            LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_STORE, LIBXSMM_BITS)((void**)pv_registry, (void*)new_registry, LIBXSMM_ATOMIC_SEQ_CST);
          }
        }
        else {
          if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
            fprintf(stderr, "LIBXSMM ERROR: failed to allocate code registry!\n");
          }
          free(internal_registry_keys);
          free(new_registry);
        }
      }
#if defined(LIBXSMM_TRACE)
      else if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
        fprintf(stderr, "LIBXSMM ERROR: failed to initialize TRACE (error #%i)!\n", init_code);
      }
    }
    else if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
      fprintf(stderr, "LIBXSMM ERROR: failed to parse LIBXSMM_TRACE!\n");
    }
#endif
  }
#if !defined(LIBXSMM_NO_SYNC) /* release locks */
# if (0 < INTERNAL_REGLOCK_MAXN)
  for (i = 0; i < internal_reglock_count; ++i) LIBXSMM_LOCK_RELEASE(LIBXSMM_REGLOCK, &internal_reglock[i].state);
# else
  LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK_RWLOCK, &internal_reglock);
# endif
  LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK, &libxsmm_lock_global);
#endif
}


LIBXSMM_API_DEFINITION LIBXSMM_ATTRIBUTE_CTOR void libxsmm_init(void)
{
  if (0 == LIBXSMM_ATOMIC_LOAD(&internal_registry, LIBXSMM_ATOMIC_RELAXED)) {
    unsigned long long s1 = libxsmm_timer_tick(), t1; /* warm-up */
    const unsigned long long s0 = libxsmm_timer_tick(), t0 = libxsmm_timer_tick_rdtsc();
#if !defined(LIBXSMM_NO_SYNC) /* setup the locks in a thread-safe fashion */
    static int counter = 0, once = 0;
    if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&counter, 1, LIBXSMM_ATOMIC_SEQ_CST)) {
      const char *const env_trylock = getenv("LIBXSMM_TRYLOCK");
# if (0 < INTERNAL_REGLOCK_MAXN)
      int i;
# endif
      LIBXSMM_LOCK_ATTR_INIT(LIBXSMM_LOCK, &libxsmm_lock_attr_default);
      LIBXSMM_LOCK_INIT(LIBXSMM_LOCK, &libxsmm_lock_global, &libxsmm_lock_attr_default);
      if (0 == env_trylock || 0 == *env_trylock) { /* LIBXSMM_TRYLOCK not present in environment */
        internal_reglock_count = INTERNAL_REGLOCK_MAXN;
      }
      else { /* LIBXSMM_TRYLOCK environment variable specified */
        internal_reglock_count = (0 != atoi(env_trylock) ? 1 : (INTERNAL_REGLOCK_MAXN));
        internal_dispatch_trylock_locked = 1;
      }
# if (0 < INTERNAL_REGLOCK_MAXN)
      assert(1 <= internal_reglock_count);
      for (i = 0; i < internal_reglock_count; ++i) LIBXSMM_LOCK_INIT(LIBXSMM_REGLOCK, &internal_reglock[i].state, &libxsmm_lock_attr_default);
# else
      LIBXSMM_LOCK_ATTR_INIT(LIBXSMM_LOCK_RWLOCK, &internal_reglock_attr);
      LIBXSMM_LOCK_INIT(LIBXSMM_LOCK_RWLOCK, &internal_reglock, &internal_reglock_attr);
# endif
      once = 1;
    }
    else while (1) {
      if (0 != once) break;
      else LIBXSMM_SYNC_PAUSE;
    }
#endif
    internal_init();
    s1 = libxsmm_timer_tick(); t1 = libxsmm_timer_tick_rdtsc(); /* final timings */
    if (0 != LIBXSMM_FEQ(0, libxsmm_timer_scale) && s0 != s1 && t0 != t1) {
      libxsmm_timer_scale = libxsmm_timer_duration(s0, s1) / (t0 < t1 ? (t1 - t0) : (t0 - t1));
    }
  }
}


LIBXSMM_API
#if defined(__GNUC__)
LIBXSMM_ATTRIBUTE(no_instrument_function)
#endif
void libxsmm_finalize(void);

LIBXSMM_API_DEFINITION LIBXSMM_ATTRIBUTE_DTOR void libxsmm_finalize(void)
{
  libxsmm_code_pointer* registry = (libxsmm_code_pointer*)LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_LOAD, LIBXSMM_BITS)(&internal_registry, LIBXSMM_ATOMIC_SEQ_CST);
  if (0 != registry) {
    int i;
#if !defined(LIBXSMM_NO_SYNC)
    LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK, &libxsmm_lock_global);
    /* acquire locks and thereby shortcut lazy initialization later on */
# if (0 < INTERNAL_REGLOCK_MAXN)
    for (i = 0; i < internal_reglock_count; ++i) LIBXSMM_LOCK_ACQUIRE(LIBXSMM_REGLOCK, &internal_reglock[i].state);
# else
    LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK_RWLOCK, &internal_reglock);
# endif
#endif
    registry = (libxsmm_code_pointer*)LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_LOAD, LIBXSMM_BITS)(&internal_registry, LIBXSMM_ATOMIC_RELAXED);

    if (0 != registry) {
      libxsmm_kernel_info *const registry_keys = internal_registry_keys;
      internal_registry_nbytes = (LIBXSMM_CAPACITY_REGISTRY) * (sizeof(libxsmm_code_pointer) + sizeof(libxsmm_kernel_info));

      /* serves as an id to invalidate the thread-local cache; never decremented */
      ++internal_teardown;
#if defined(LIBXSMM_TRACE)
      i = libxsmm_trace_finalize();
      if (EXIT_SUCCESS != i && 0 != libxsmm_verbosity) { /* library code is expected to be mute */
        fprintf(stderr, "LIBXSMM ERROR: failed to finalize trace (error #%i)!\n", i);
      }
#endif
      libxsmm_gemm_finalize();
      libxsmm_gemm_diff_finalize();
      libxsmm_trans_finalize();
      libxsmm_hash_finalize();
      libxsmm_dnn_finalize();
#if defined(LIBXSMM_PERF)
      libxsmm_perf_finalize();
#endif
      for (i = 0; i < (LIBXSMM_CAPACITY_REGISTRY); ++i) {
        /*const*/ libxsmm_code_pointer code = registry[i];
        if (0 != code.ptr_const) {
          /* check if the registered entity is a GEMM kernel */
          if (LIBXSMM_KERNEL_KIND_MATMUL == registry_keys[i].xgemm.iflags) {
            const libxsmm_gemm_descriptor *const desc = &registry_keys[i].xgemm;
            const unsigned long long kernel_size = LIBXSMM_MNK_SIZE(desc->m, desc->n, desc->k);
            const int precision = (LIBXSMM_GEMM_PRECISION_F64 == desc->datatype ? 0 : 1);
            int bucket = 3/*huge*/;
            assert(0 < kernel_size);
            if (LIBXSMM_MNK_SIZE(internal_statistic_sml, internal_statistic_sml, internal_statistic_sml) >= kernel_size) {
              bucket = 0;
            }
            else if (LIBXSMM_MNK_SIZE(internal_statistic_med, internal_statistic_med, internal_statistic_med) >= kernel_size) {
              bucket = 1;
            }
            else if (LIBXSMM_MNK_SIZE(internal_statistic_mnk, internal_statistic_mnk, internal_statistic_mnk) >= kernel_size) {
              bucket = 2;
            }
            if (0 == (LIBXSMM_CODE_STATIC & code.uval)) { /* count whether kernel is static or JIT-code */
              ++internal_statistic[precision][bucket].njit;
            }
            else {
              ++internal_statistic[precision][bucket].nsta;
            }
          }
          else if (LIBXSMM_KERNEL_KIND_MCOPY == registry_keys[i].xgemm.iflags) {
            ++internal_statistic_num_mcopy;
          }
          else if (LIBXSMM_KERNEL_KIND_TCOPY == registry_keys[i].xgemm.iflags) {
            ++internal_statistic_num_tcopy;
          }
          else {
            fprintf(stderr, "LIBXSMM ERROR: code registry is corrupted!\n");
          }
          if (0 == (LIBXSMM_CODE_STATIC & code.uval)) { /* check for allocated/generated JIT-code */
            void* buffer = 0;
            size_t size = 0;
#if defined(LIBXSMM_HASH_COLLISION)
            code.uval &= ~LIBXSMM_HASH_COLLISION; /* clear collision flag */
#endif
            if (EXIT_SUCCESS == libxsmm_get_malloc_xinfo(code.ptr_const, &size, 0/*flags*/, &buffer)) {
              libxsmm_xfree(code.ptr_const);
              internal_registry_nbytes += (unsigned int)(size + (((char*)code.ptr_const) - (char*)buffer));
            }
          }
        }
      }
      /* make internal registry globally unavailable */
      LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_STORE_ZERO, LIBXSMM_BITS)(&internal_registry, LIBXSMM_ATOMIC_SEQ_CST);
      internal_registry_keys = 0;
      free(registry_keys);
      free(registry);
    }
#if !defined(LIBXSMM_NO_SYNC) /* LIBXSMM_LOCK_RELEASE, but no LIBXSMM_LOCK_DESTROY */
# if (0 < INTERNAL_REGLOCK_MAXN)
    for (i = 0; i < internal_reglock_count; ++i) LIBXSMM_LOCK_RELEASE(LIBXSMM_REGLOCK, &internal_reglock[i].state);
# else
    LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK_RWLOCK, &internal_reglock);
# endif
    LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK, &libxsmm_lock_global);
#endif
  }
}


LIBXSMM_API_DEFINITION int libxsmm_get_target_archid(void)
{
  LIBXSMM_INIT
#if !defined(__MIC__)
  return libxsmm_target_archid;
#else /* no JIT support */
  return LIBXSMM_MIN(libxsmm_target_archid, LIBXSMM_X86_SSE4);
#endif
}


LIBXSMM_API_DEFINITION void libxsmm_set_target_archid(int id)
{
  int target_archid = LIBXSMM_TARGET_ARCH_UNKNOWN;
  switch (id) {
    case LIBXSMM_X86_AVX512_CORE:
    case LIBXSMM_X86_AVX512_KNM:
    case LIBXSMM_X86_AVX512_MIC:
    case LIBXSMM_X86_AVX512:
    case LIBXSMM_X86_AVX2:
    case LIBXSMM_X86_AVX:
    case LIBXSMM_X86_SSE4:
    case LIBXSMM_X86_SSE3:
    case LIBXSMM_TARGET_ARCH_GENERIC: {
      target_archid = id;
    } break;
    default: if (LIBXSMM_X86_GENERIC <= id) {
      target_archid = LIBXSMM_X86_GENERIC;
    }
    else {
      target_archid = libxsmm_cpuid();
    }
  }
  LIBXSMM_ATOMIC_STORE(&libxsmm_target_archid, target_archid, LIBXSMM_ATOMIC_RELAXED);
  if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
    const int cpuid = libxsmm_cpuid();
    if (cpuid < target_archid) {
      const char *const target_arch = internal_get_target_arch(target_archid);
      fprintf(stderr, "LIBXSMM WARNING: \"%s\" code will fail to run on \"%s\"!\n",
        target_arch, internal_get_target_arch(cpuid));
    }
  }
}


LIBXSMM_API_DEFINITION const char* libxsmm_get_target_arch(void)
{
  LIBXSMM_INIT
  return internal_get_target_arch(libxsmm_target_archid);
}


/* function serves as a helper for implementing the Fortran interface */
LIBXSMM_API const char* get_target_arch(int* length);
LIBXSMM_API_DEFINITION const char* get_target_arch(int* length)
{
  const char *const arch = libxsmm_get_target_arch();
  /* valid here since function is not in the public interface */
  assert(0 != arch && 0 != length);
  *length = (int)strlen(arch);
  return arch;
}


LIBXSMM_API_DEFINITION void libxsmm_set_target_arch(const char* arch)
{
  int target_archid = LIBXSMM_TARGET_ARCH_UNKNOWN;
  if (0 != arch && 0 != *arch) {
    const int jit = atoi(arch);
    if (0 == strcmp("0", arch)) {
      target_archid = LIBXSMM_TARGET_ARCH_GENERIC;
    }
    else if (1 < jit) {
      target_archid = LIBXSMM_X86_GENERIC + jit;
    }
    else if (0 == strcmp("icl", arch) || 0 == strcmp("icx", arch)) {
      target_archid = LIBXSMM_X86_AVX512_ICL;
    }
    else if (0 == strcmp("skx", arch) || 0 == strcmp("skl", arch)) {
      target_archid = LIBXSMM_X86_AVX512_CORE;
    }
    else if (0 == strcmp("knm", arch)) {
      target_archid = LIBXSMM_X86_AVX512_KNM;
    }
    else if (0 == strcmp("knl", arch) || 0 == strcmp("mic", arch)) {
      target_archid = LIBXSMM_X86_AVX512_MIC;
    }
    else if (0 == strcmp("avx3", arch) || 0 == strcmp("avx512", arch)) {
      target_archid = LIBXSMM_X86_AVX512;
    }
    else if (0 == strcmp("hsw", arch) || 0 == strcmp("avx2", arch)) {
      target_archid = LIBXSMM_X86_AVX2;
    }
    else if (0 == strcmp("snb", arch) || 0 == strcmp("avx", arch)) {
      target_archid = LIBXSMM_X86_AVX;
    }
    else if (0 == strcmp("wsm", arch) || 0 == strcmp("nhm", arch) || 0 == strcmp("sse4", arch) || 0 == strcmp("sse4_2", arch) || 0 == strcmp("sse4.2", arch)) {
      target_archid = LIBXSMM_X86_SSE4;
    }
    else if (0 == strcmp("sse", arch) || 0 == strcmp("sse3", arch)) {
      target_archid = LIBXSMM_X86_SSE3;
    }
    else if (0 == strcmp("x86", arch) || 0 == strcmp("sse2", arch)) {
      target_archid = LIBXSMM_X86_GENERIC;
    }
    else if (0 == strcmp("generic", arch) || 0 == strcmp("none", arch)) {
      target_archid = LIBXSMM_TARGET_ARCH_GENERIC;
    }
  }

  if (LIBXSMM_TARGET_ARCH_UNKNOWN == target_archid || LIBXSMM_X86_AVX512_ICL < target_archid) {
    target_archid = libxsmm_cpuid();
  }
  else if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
    const int cpuid = libxsmm_cpuid();
    if (cpuid < target_archid) {
      const char *const target_arch = internal_get_target_arch(target_archid);
      fprintf(stderr, "LIBXSMM WARNING: \"%s\" code will fail to run on \"%s\"!\n",
        target_arch, internal_get_target_arch(cpuid));
    }
  }
  LIBXSMM_ATOMIC_STORE(&libxsmm_target_archid, target_archid, LIBXSMM_ATOMIC_RELAXED);
}


LIBXSMM_API_DEFINITION int libxsmm_get_verbosity(void)
{
  LIBXSMM_INIT
  return libxsmm_verbosity;
}


LIBXSMM_API_DEFINITION void libxsmm_set_verbosity(int level)
{
  LIBXSMM_INIT
  LIBXSMM_ATOMIC_STORE(&libxsmm_verbosity, level, LIBXSMM_ATOMIC_RELAXED);
}


LIBXSMM_API_DEFINITION int libxsmm_get_dispatch_trylock(void)
{
  LIBXSMM_INIT
  return 1 == internal_reglock_count ? 1 : 0;
}


LIBXSMM_API_DEFINITION void libxsmm_set_dispatch_trylock(int trylock)
{
#if defined(LIBXSMM_NO_SYNC)
  LIBXSMM_UNUSED(trylock);
#else
  LIBXSMM_INIT
  if (0 == internal_dispatch_trylock_locked) { /* LIBXSMM_TRYLOCK environment takes precedence */
    LIBXSMM_ATOMIC_STORE(&internal_reglock_count, 0 != trylock ? 1 : INTERNAL_REGLOCK_MAXN, LIBXSMM_ATOMIC_RELAXED);
  }
#endif
}


LIBXSMM_API_DEFINITION libxsmm_gemm_prefetch_type libxsmm_get_gemm_auto_prefetch(void)
{
  return (libxsmm_gemm_prefetch_type)libxsmm_gemm_auto_prefetch;
}


LIBXSMM_API_DEFINITION void libxsmm_set_gemm_auto_prefetch(libxsmm_gemm_prefetch_type strategy)
{
  if (0 == internal_gemm_auto_prefetch_locked) { /* LIBXSMM_GEMM_PREFETCH environment takes precedence */
    LIBXSMM_ATOMIC_STORE(&libxsmm_gemm_auto_prefetch_default, strategy, LIBXSMM_ATOMIC_RELAXED);
    LIBXSMM_ATOMIC_STORE(&libxsmm_gemm_auto_prefetch, strategy, LIBXSMM_ATOMIC_RELAXED);
  }
}


LIBXSMM_API_DEFINITION unsigned char libxsmm_typesize(libxsmm_datatype datatype)
{
  switch (datatype) {
    case LIBXSMM_DATATYPE_F64: return 8;
    case LIBXSMM_DATATYPE_F32: return 4;
    case LIBXSMM_DATATYPE_I32: return 4;
    case LIBXSMM_DATATYPE_I16: return 2;
    case LIBXSMM_DATATYPE_I8:  return 1;
  }
  return 0;
}


LIBXSMM_API const char* internal_get_typename(int /*datatype*/);
LIBXSMM_API_DEFINITION const char* internal_get_typename(int datatype)
{
  switch (datatype) {
    case LIBXSMM_DATATYPE_F64: return "f64";
    case LIBXSMM_DATATYPE_F32: return "f32";
    case LIBXSMM_DATATYPE_I32: return "i32";
    case LIBXSMM_DATATYPE_I16: return "i16";
    case LIBXSMM_DATATYPE_I8:  return "i8";
  }
  return "void";
}


LIBXSMM_API const char* internal_get_typesize_string(size_t typesize);
LIBXSMM_API_DEFINITION const char* internal_get_typesize_string(size_t typesize)
{
  static LIBXSMM_TLS char result[4];
  assert(256 > typesize);
  if (1 < typesize) {
    LIBXSMM_SNPRINTF(result, sizeof(result), "%i", (int)typesize);
  }
  else {
    result[0] = 0;
  }
  return result;
}


LIBXSMM_API_DEFINITION int libxsmm_build(const libxsmm_build_request* request, unsigned int regindex, libxsmm_code_pointer* code)
{
  int result = EXIT_SUCCESS;
#if !defined(__MIC__)
  const char *const target_arch = internal_get_target_arch(libxsmm_target_archid);
  libxsmm_generated_code generated_code = { 0 };
  char jit_name[256] = { 0 };

  /* large enough temporary buffer for generated code */
#if defined(NDEBUG)
  char jit_buffer[LIBXSMM_CODE_MAXSIZE];
  generated_code.generated_code = jit_buffer;
  generated_code.buffer_size = sizeof(jit_buffer);
#else
  generated_code.generated_code = malloc(LIBXSMM_CODE_MAXSIZE);
  generated_code.buffer_size = (0 != generated_code.generated_code ? LIBXSMM_CODE_MAXSIZE : 0);
#endif
  /* setup code generation */
  generated_code.code_type = 2;

  assert(0 != request && 0 != libxsmm_target_archid);
  assert(0 != code && 0 == code->ptr_const);

  switch (request->kind) { /* generate kernel */
    case LIBXSMM_BUILD_KIND_GEMM: { /* small MxM kernel */
      assert(0 != request->descriptor.gemm);
      if (0 < request->descriptor.gemm->m   && 0 < request->descriptor.gemm->n   && 0 < request->descriptor.gemm->k &&
          0 < request->descriptor.gemm->lda && 0 < request->descriptor.gemm->ldb && 0 < request->descriptor.gemm->ldc)
      {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_gemm_kernel, &generated_code, request->descriptor.gemm, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const int uid = libxsmm_gemm_prefetch2uid((libxsmm_gemm_prefetch_type)request->descriptor.gemm->prefetch);
          const char *const tname = internal_get_typename(request->descriptor.gemm->datatype);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_%s_%c%c_%ux%ux%u_%u_%u_%u_a%i_b%i_p%i.mxm", target_arch, tname,
            0 == (LIBXSMM_GEMM_FLAG_TRANS_A & request->descriptor.gemm->flags) ? 'n' : 't',
            0 == (LIBXSMM_GEMM_FLAG_TRANS_B & request->descriptor.gemm->flags) ? 'n' : 't',
            (unsigned int)request->descriptor.gemm->m,   (unsigned int)request->descriptor.gemm->n,   (unsigned int)request->descriptor.gemm->k,
            (unsigned int)request->descriptor.gemm->lda, (unsigned int)request->descriptor.gemm->ldb, (unsigned int)request->descriptor.gemm->ldc,
            request->descriptor.gemm->alpha, request->descriptor.gemm->beta, uid);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_SRSOA: { /* sparse SOA kernel, CSR format */
      assert(0 != request->descriptor.srsoa && 0 != request->descriptor.srsoa->gemm);
      assert(0 != request->descriptor.srsoa->row_ptr && 0 != request->descriptor.srsoa->column_idx && 0 != request->descriptor.srsoa->values);
      /* only floating point */
      if (LIBXSMM_GEMM_PRECISION_F64 == request->descriptor.srsoa->gemm->datatype || LIBXSMM_GEMM_PRECISION_F32 == request->descriptor.srsoa->gemm->datatype) {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_spgemm_csr_soa_kernel, &generated_code, request->descriptor.srsoa->gemm, target_arch,
          request->descriptor.srsoa->row_ptr, request->descriptor.srsoa->column_idx, request->descriptor.srsoa->values);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const int uid = libxsmm_gemm_prefetch2uid((libxsmm_gemm_prefetch_type)request->descriptor.srsoa->gemm->prefetch);
          const char *const tname = internal_get_typename(request->descriptor.srsoa->gemm->datatype);
          const unsigned int nnz = ((unsigned int)request->descriptor.srsoa->gemm->lda == 0) ?
            request->descriptor.srsoa->row_ptr[request->descriptor.srsoa->gemm->m] : request->descriptor.srsoa->row_ptr[request->descriptor.srsoa->gemm->k];
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_%s_%c%c_%ux%ux%u_%u_%u_%u_a%i_b%i_p%i_nnz%u.srsoa", target_arch, tname,
            0 == (LIBXSMM_GEMM_FLAG_TRANS_A & request->descriptor.srsoa->gemm->flags) ? 'n' : 't',
            0 == (LIBXSMM_GEMM_FLAG_TRANS_B & request->descriptor.srsoa->gemm->flags) ? 'n' : 't',
            (unsigned int)request->descriptor.srsoa->gemm->m,   (unsigned int)request->descriptor.srsoa->gemm->n,   (unsigned int)request->descriptor.srsoa->gemm->k,
            (unsigned int)request->descriptor.srsoa->gemm->lda, (unsigned int)request->descriptor.srsoa->gemm->ldb, (unsigned int)request->descriptor.srsoa->gemm->ldc,
            request->descriptor.srsoa->gemm->alpha, request->descriptor.srsoa->gemm->beta, uid, nnz);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_SCSOA: { /* sparse SOA kernel, CSC format */
      assert(0 != request->descriptor.scsoa && 0 != request->descriptor.scsoa->gemm);
      assert(0 != request->descriptor.scsoa->row_idx && 0 != request->descriptor.scsoa->column_ptr && 0 != request->descriptor.scsoa->values);
      /* only floating point */
      if (LIBXSMM_GEMM_PRECISION_F64 == request->descriptor.scsoa->gemm->datatype || LIBXSMM_GEMM_PRECISION_F32 == request->descriptor.scsoa->gemm->datatype) {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_spgemm_csc_soa_kernel, &generated_code, request->descriptor.scsoa->gemm, target_arch,
          request->descriptor.scsoa->row_idx, request->descriptor.scsoa->column_ptr, request->descriptor.scsoa->values);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const int uid = libxsmm_gemm_prefetch2uid((libxsmm_gemm_prefetch_type)request->descriptor.scsoa->gemm->prefetch);
          const char *const tname = internal_get_typename(request->descriptor.scsoa->gemm->datatype);
          const unsigned int nnz = ((unsigned int)request->descriptor.srsoa->gemm->lda == 0) ?
            request->descriptor.scsoa->column_ptr[request->descriptor.scsoa->gemm->k] : request->descriptor.scsoa->column_ptr[request->descriptor.scsoa->gemm->n];
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_%s_%c%c_%ux%ux%u_%u_%u_%u_a%i_b%i_p%i_nnz%u.scsoa", target_arch, tname,
            0 == (LIBXSMM_GEMM_FLAG_TRANS_A & request->descriptor.scsoa->gemm->flags) ? 'n' : 't',
            0 == (LIBXSMM_GEMM_FLAG_TRANS_B & request->descriptor.scsoa->gemm->flags) ? 'n' : 't',
            (unsigned int)request->descriptor.scsoa->gemm->m,   (unsigned int)request->descriptor.scsoa->gemm->n,   (unsigned int)request->descriptor.scsoa->gemm->k,
            (unsigned int)request->descriptor.scsoa->gemm->lda, (unsigned int)request->descriptor.scsoa->gemm->ldb, (unsigned int)request->descriptor.scsoa->gemm->ldc,
            request->descriptor.scsoa->gemm->alpha, request->descriptor.scsoa->gemm->beta, uid, nnz);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_SREG: { /* sparse register kernel */
      assert(0 != request->descriptor.sreg && 0 != request->descriptor.sreg->gemm);
      assert(0 != request->descriptor.sreg->row_ptr && 0 != request->descriptor.sreg->column_idx && 0 != request->descriptor.sreg->values);
#if 1
      if (LIBXSMM_GEMM_PRECISION_F64 == request->descriptor.sreg->gemm->flags) { /* only double-precision */
#endif
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_spgemm_csr_reg_kernel, &generated_code, request->descriptor.sreg->gemm, target_arch,
          request->descriptor.sreg->row_ptr, request->descriptor.sreg->column_idx,
          (const double*)request->descriptor.sreg->values);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const int uid = libxsmm_gemm_prefetch2uid((libxsmm_gemm_prefetch_type)request->descriptor.sreg->gemm->prefetch);
          const char *const tname = internal_get_typename(request->descriptor.sreg->gemm->datatype);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_%s_%c%c_%ux%ux%u_%u_%u_%u_a%i_b%i_p%i.sreg", target_arch, tname,
            0 == (LIBXSMM_GEMM_FLAG_TRANS_A & request->descriptor.sreg->gemm->flags) ? 'n' : 't',
            0 == (LIBXSMM_GEMM_FLAG_TRANS_B & request->descriptor.sreg->gemm->flags) ? 'n' : 't',
            (unsigned int)request->descriptor.sreg->gemm->m,   (unsigned int)request->descriptor.sreg->gemm->n,   (unsigned int)request->descriptor.sreg->gemm->k,
            (unsigned int)request->descriptor.sreg->gemm->lda, (unsigned int)request->descriptor.sreg->gemm->ldb, (unsigned int)request->descriptor.sreg->gemm->ldc,
            request->descriptor.sreg->gemm->alpha, request->descriptor.sreg->gemm->beta, uid);
        }
#if 1
      }
#endif
    } break;
    case LIBXSMM_BUILD_KIND_CFWD: { /* forward convolution */
      assert(0 != request->descriptor.cfwd);
      if (0 < request->descriptor.cfwd->kw && 0 < request->descriptor.cfwd->kh &&
          0 != request->descriptor.cfwd->stride_w && 0 != request->descriptor.cfwd->stride_h)
      {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_convolution_forward_kernel, &generated_code, request->descriptor.cfwd, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const char *const precision_in = internal_get_typename(request->descriptor.cfwd->datatype);
          const char *const precision_out = internal_get_typename(request->descriptor.cfwd->datatype_itm);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_fwd_%s_%s_%ux%u_%ux%uu_s%ii%io_vl%ui%uo_ri%ux%u_ro%ux%u_r%ux%u_p%i_f%i.conv",
            target_arch/*code path name*/, precision_in, precision_out,
            (unsigned int)request->descriptor.cfwd->kw/*kernel width*/, (unsigned int)request->descriptor.cfwd->kh/*kernel height*/,
            (unsigned int)request->descriptor.cfwd->unroll_kw/*width*/, (unsigned int)request->descriptor.cfwd->unroll_kh/*height*/,
            (int)request->descriptor.cfwd->stride_w/*input offset*/, (int)request->descriptor.cfwd->stride_h/*output offsets*/,
            (unsigned int)request->descriptor.cfwd->ifm_block/*VLEN*/, (unsigned int)request->descriptor.cfwd->ofm_block/*VLEN*/,
            (unsigned int)request->descriptor.cfwd->ifw_padded, (unsigned int)request->descriptor.cfwd->ifh_padded,
            (unsigned int)request->descriptor.cfwd->ofw_padded/*1D and 2D register block*/,
            (unsigned int)request->descriptor.cfwd->ofh_padded/*2D register block*/,
            (unsigned int)request->descriptor.cfwd->ofw_rb/*register block ofw*/,
            (unsigned int)request->descriptor.cfwd->ofh_rb/*register block ofh*/,
            (int)request->descriptor.cfwd->prefetch/*binary OR'd prefetch flags*/,
            (int)request->descriptor.cfwd->format/*binary OR'd format flags*/);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_CBWD: { /* backward convolution */
      assert(0 != request->descriptor.cbwd);
      if (0 < request->descriptor.cbwd->kw && 0 < request->descriptor.cbwd->kh &&
          0 != request->descriptor.cbwd->stride_w && 0 != request->descriptor.cbwd->stride_h)
      {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_convolution_backward_kernel, &generated_code, request->descriptor.cbwd, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const char *const precision_in = internal_get_typename(request->descriptor.cbwd->datatype);
          const char *const precision_out = internal_get_typename(request->descriptor.cbwd->datatype_itm);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_bwd_%s_%s_%ux%u_%ux%uu_s%ii%io_vl%ui%uo_ri%ux%u_ro%ux%u_r%ux%u_p%i_f%i.conv",
            target_arch/*code path name*/, precision_in, precision_out,
            (unsigned int)request->descriptor.cbwd->kw/*kernel width*/, (unsigned int)request->descriptor.cbwd->kh/*kernel height*/,
            (unsigned int)request->descriptor.cbwd->unroll_kw/*width*/, (unsigned int)request->descriptor.cbwd->unroll_kh/*height*/,
            (int)request->descriptor.cbwd->stride_w/*input offset*/, (int)request->descriptor.cbwd->stride_h/*output offsets*/,
            (unsigned int)request->descriptor.cbwd->ifm_block/*VLEN*/, (unsigned int)request->descriptor.cbwd->ofm_block/*VLEN*/,
            (unsigned int)request->descriptor.cbwd->ifw_padded, (unsigned int)request->descriptor.cbwd->ifh_padded,
            (unsigned int)request->descriptor.cbwd->ofw_padded/*1D and 2D register block*/,
            (unsigned int)request->descriptor.cbwd->ofh_padded/*2D register block*/,
            (unsigned int)request->descriptor.cbwd->ofw_rb/*register block ofw*/,
            (unsigned int)request->descriptor.cbwd->ofh_rb/*register block ofh*/,
            (int)request->descriptor.cbwd->prefetch/*binary OR'd prefetch flags*/,
            (int)request->descriptor.cbwd->format/*binary OR'd format flags*/);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_CUPD: { /* convolution update weights */
      assert(0 != request->descriptor.cupd);
      if (0 < request->descriptor.cupd->kw &&
          0 != request->descriptor.cupd->stride_w && 0 != request->descriptor.cupd->stride_h)
      {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_convolution_weight_update_kernel, &generated_code, request->descriptor.cupd, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const char *const precision_in = internal_get_typename(request->descriptor.cupd->datatype);
          const char *const precision_out = internal_get_typename(request->descriptor.cupd->datatype_itm);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_upd_%s_%s_%ux%u_%uu_s%ii%io_vl%ui%uo_ri%ux%u_ro%ux%u_r%ux%u_of%uu%ux%uu%u_if%uu_t%u_p%i_f%i.conv",
            target_arch/*code path name*/, precision_in, precision_out,
            (unsigned int)request->descriptor.cupd->kw/*kernel width*/, (unsigned int)request->descriptor.cupd->kh/*kernel height*/,
            (unsigned int)request->descriptor.cupd->unroll_kw/*width*/,
            (int)request->descriptor.cupd->stride_w/*input offset*/, (int)request->descriptor.cupd->stride_h/*output offsets*/,
            (unsigned int)request->descriptor.cupd->ifm_block/*VLEN*/, (unsigned int)request->descriptor.cupd->ofm_block/*VLEN*/,
            (unsigned int)request->descriptor.cupd->ifw_padded, (unsigned int)request->descriptor.cupd->ifh_padded,
            (unsigned int)request->descriptor.cupd->ofw_padded/*1D and 2D register block*/,
            (unsigned int)request->descriptor.cupd->ofh_padded/*2D register block*/,
            (unsigned int)request->descriptor.cupd->ofw_rb/*register block ofw*/,
            (unsigned int)request->descriptor.cupd->ofh_rb/*register block ofh*/,
            (unsigned int)request->descriptor.cupd->ofw/*ofw*/, (unsigned int)request->descriptor.cupd->ofw_unroll/*ofw_unroll*/,
            (unsigned int)request->descriptor.cupd->ofh/*ofh*/, (unsigned int)request->descriptor.cupd->ofh_unroll/*ofh_unroll*/,
            (unsigned int)request->descriptor.cupd->ifm_unroll/*ifm unroll*/,
            (unsigned int)request->descriptor.cupd->transpose_ofw_ifm/*transpose_ofw_ifm*/,
            (int)request->descriptor.cupd->prefetch/*binary OR'd prefetch flags*/,
            (int)request->descriptor.cupd->format/*binary OR'd format flags*/);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_CWFWD: { /* convolution Winograd forward */
      assert(0 != request->descriptor.cwino);
      if (0 < request->descriptor.cwino->itiles && 0 < request->descriptor.cwino->jtiles &&
          0 < request->descriptor.cwino->bimg && 0 < request->descriptor.cwino->ur)
      {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_convolution_winograd_forward_kernel, &generated_code, request->descriptor.cwino, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const char *const precision_in = internal_get_typename(LIBXSMM_DNN_DATATYPE_F32);
          const char *const precision_out = internal_get_typename(LIBXSMM_DNN_DATATYPE_F32);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_wfwd_%s_%s_t%ux%u_mb%u_u%u_p%i.convwino",
            target_arch/*code path name*/, precision_in, precision_out,
            (unsigned int)request->descriptor.cwino->itiles/*itiles*/,
            (unsigned int)request->descriptor.cwino->jtiles/*jtiles*/,
            (unsigned int)request->descriptor.cwino->bimg/*image block*/,
            (unsigned int)request->descriptor.cwino->ur/*unrolling*/,
            (int)request->descriptor.cwino->prefetch/*binary OR'd prefetch flags*/);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_CWBWD: { /* convolution Winograd backward */
      assert(0 != request->descriptor.cwino);
      if (0 < request->descriptor.cwino->itiles && 0 < request->descriptor.cwino->jtiles &&
          0 < request->descriptor.cwino->bimg && 0 < request->descriptor.cwino->ur)
      {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_convolution_winograd_forward_kernel, &generated_code, request->descriptor.cwino, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const char *const precision_in = internal_get_typename(LIBXSMM_DNN_DATATYPE_F32);
          const char *const precision_out = internal_get_typename(LIBXSMM_DNN_DATATYPE_F32);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_wbwd_%s_%s_t%ux%u_mb%u_u%u_p%i.convwino",
            target_arch/*code path name*/, precision_in, precision_out,
            (unsigned int)request->descriptor.cwino->itiles/*itiles*/,
            (unsigned int)request->descriptor.cwino->jtiles/*jtiles*/,
            (unsigned int)request->descriptor.cwino->bimg/*image block*/,
            (unsigned int)request->descriptor.cwino->ur/*unrolling*/,
            (int)request->descriptor.cwino->prefetch/*binary OR'd prefetch flags*/);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_CWUPD: { /* convolution Winograd update */
      assert(0 != request->descriptor.cwino);
      if (0 < request->descriptor.cwino->itiles && 0 < request->descriptor.cwino->jtiles &&
          0 < request->descriptor.cwino->bimg && 0 < request->descriptor.cwino->ur)
      {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_convolution_winograd_weight_update_kernel, &generated_code, request->descriptor.cwino, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const char *const precision_in = internal_get_typename(LIBXSMM_DNN_DATATYPE_F32);
          const char *const precision_out = internal_get_typename(LIBXSMM_DNN_DATATYPE_F32);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_wupd_%s_%s_t%ux%u_mb%u_u%u_p%i.convwino",
            target_arch/*code path name*/, precision_in, precision_out,
            (unsigned int)request->descriptor.cwino->itiles/*itiles*/,
            (unsigned int)request->descriptor.cwino->jtiles/*jtiles*/,
            (unsigned int)request->descriptor.cwino->bimg/*image block*/,
            (unsigned int)request->descriptor.cwino->ur/*unrolling*/,
            (int)request->descriptor.cwino->prefetch/*binary OR'd prefetch flags*/);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_MCOPY: { /* matcopy kernel */
      assert(0 != request->descriptor.matcopy);
      if (4 == request->descriptor.matcopy->typesize || 8 == request->descriptor.matcopy->typesize
       || 2 == request->descriptor.matcopy->typesize || 1 == request->descriptor.matcopy->typesize)
      {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_matcopy_kernel, &generated_code, request->descriptor.matcopy, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const char *const tsizename = internal_get_typesize_string(request->descriptor.matcopy->typesize);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_tsize%s_%ux%u_%ux%u_p%u.mcopy", target_arch, tsizename,
            request->descriptor.matcopy->m, request->descriptor.matcopy->n,
            request->descriptor.matcopy->ldi, request->descriptor.matcopy->ldo,
            (unsigned int)request->descriptor.matcopy->prefetch);
        }
      }
    } break;
    case LIBXSMM_BUILD_KIND_TRANS: { /* transpose kernel */
      assert(0 != request->descriptor.trans);
      if (4 == request->descriptor.trans->typesize || 8 == request->descriptor.trans->typesize) {
        LIBXSMM_NO_OFFLOAD(void, libxsmm_generator_transpose_kernel, &generated_code, request->descriptor.trans, target_arch);
# if !defined(LIBXSMM_VTUNE)
        if (0 > libxsmm_verbosity)
# endif
        {
          const char *const tsizename = internal_get_typesize_string(request->descriptor.trans->typesize);
          /* adopt scheme which allows kernel names of LIBXSMM to appear in order (Intel VTune, etc.) */
          LIBXSMM_SNPRINTF(jit_name, sizeof(jit_name), "libxsmm_%s_tsize%s_%ux%u.trans", target_arch, tsizename,
            request->descriptor.trans->m, request->descriptor.trans->n);
        }
      }
    } break;
# if !defined(NDEBUG) /* library code is expected to be mute */
    default: { /* unknown kind */
      static int error_once = 0;
      if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) {
        fprintf(stderr, "LIBXSMM ERROR: invalid build request discovered!\n");
      }
      result = EXIT_FAILURE;
    }
# endif
  }

  /* handle an eventual error in the else-branch */
  if (0 != generated_code.generated_code) {
    if (0 == generated_code.last_error) { /* no error raised */
      if (0 < generated_code.code_size) { /* sanity check */
        /* attempt to create executable buffer */
        result = libxsmm_xmalloc(&code->pmm, generated_code.code_size, 0/*auto*/,
          /* flag must be a superset of what's populated by libxsmm_malloc_attrib */
          LIBXSMM_MALLOC_FLAG_RWX, &regindex, sizeof(regindex));
        if (EXIT_SUCCESS == result) { /* check for success */
          assert(0 != code->pmm && 0 == (LIBXSMM_CODE_STATIC & code->uval));
          assert(0 != generated_code.generated_code/*sanity check*/);
          /* copy temporary buffer into the prepared executable buffer */
          memcpy(code->pmm, generated_code.generated_code, generated_code.code_size);
          /* attribute/protect buffer and revoke unnecessary flags */
          result = libxsmm_malloc_attrib(&code->pmm, LIBXSMM_MALLOC_FLAG_X, jit_name);
        }
      }
    }
    else {
# if !defined(LIBXSMM_VERBOSE_BACKEND) /* avoid duplicated error messages */
      if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
        static int error_once = 0;
        if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) {
          LIBXSMM_NO_OFFLOAD(int, fprintf, stderr, "LIBXSMM ERROR: %s\n",
            LIBXSMM_NO_OFFLOAD(const char*, libxsmm_strerror, generated_code.last_error));
        }
      }
# endif
      result = EXIT_FAILURE;
    }
# if !defined(NDEBUG)
    free(generated_code.generated_code); /* free temporary/initial code buffer */
# endif
  }
#else /* unsupported platform */
  LIBXSMM_UNUSED(request); LIBXSMM_UNUSED(regindex); LIBXSMM_UNUSED(code);
  /* libxsmm_get_target_arch also serves as a runtime check whether JIT is available or not */
  if (LIBXSMM_X86_AVX <= libxsmm_target_archid) result = EXIT_FAILURE;
#endif
  return result;
}


LIBXSMM_API_INLINE libxsmm_code_pointer internal_find_code(const libxsmm_gemm_descriptor* descriptor)
{
  libxsmm_code_pointer flux_entry = { 0 };
  unsigned int hash, i0, i = 0, mode = 0, diff = 1;
#if !defined(NDEBUG)
  const libxsmm_gemm_descriptor* refdesc = 0;
#endif
#if defined(LIBXSMM_CAPACITY_CACHE) && (0 < (LIBXSMM_CAPACITY_CACHE))
  static LIBXSMM_TLS struct {
    libxsmm_gemm_descriptor keys[LIBXSMM_CAPACITY_CACHE];
    libxsmm_code_pointer code[LIBXSMM_CAPACITY_CACHE];
    unsigned int hit, id;
  } cache;
  unsigned int cache_index;
  assert(0 != descriptor);
  /* search small cache starting with the last hit on record */
  cache_index = libxsmm_gemm_diffn(descriptor, &cache.keys, cache.hit, LIBXSMM_CAPACITY_CACHE, LIBXSMM_GEMM_DESCRIPTOR_SIZE);
  if ((LIBXSMM_CAPACITY_CACHE) > cache_index && cache.id == internal_teardown) { /* cache hit, and valid */
    flux_entry = cache.code[cache_index];
    cache.hit = cache_index;
#if !defined(NDEBUG)
    if (0 == (LIBXSMM_CODE_STATIC & flux_entry.uval)) { /* JIT only */
      void* extra = 0;
# if defined(LIBXSMM_HASH_COLLISION)
      flux_entry.uval &= ~LIBXSMM_HASH_COLLISION; /* clear collision flag */
# endif
      if (EXIT_SUCCESS == libxsmm_get_malloc_xinfo(flux_entry.ptr_const, 0/*size*/, 0/*flags*/, &extra) && 0 != extra) {
        refdesc = &internal_registry_keys[*((const unsigned int*)extra)].xgemm;
      }
    }
#endif
  }
  else
#else
  assert(0 != descriptor);
#endif
  {
    assert(0 != internal_registry);
    /* check if the requested xGEMM is already JITted */
    LIBXSMM_HASH_FUNCTION_CALL(hash, i = i0, descriptor);
    while (0 != diff) {
#if (0 < INTERNAL_REGLOCK_MAXN) || defined(LIBXSMM_NO_SYNC) /* read registered code */
      flux_entry.pmm = (void*)LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_LOAD, LIBXSMM_BITS)(&internal_registry[i].pmm, LIBXSMM_ATOMIC_RELAXED);
#else
      LIBXSMM_LOCK_ACQREAD(LIBXSMM_LOCK_RWLOCK, &internal_reglock);
      flux_entry.pmm = internal_registry[i].pmm; /* read registered code */
      LIBXSMM_LOCK_RELREAD(LIBXSMM_LOCK_RWLOCK, &internal_reglock);
#endif
      if ((0 != flux_entry.ptr_const || 1 == mode) && 2 > mode) { /* check existing entry further */
        diff = 0 != flux_entry.ptr_const ? libxsmm_gemm_diff(descriptor, &internal_registry_keys[i].xgemm) : 1;
        if (0 != diff) { /* search for code version */
          if (0 == mode) { /* transition to higher mode */
            i0 = i; /* keep current position on record */
#if defined(LIBXSMM_HASH_COLLISION)
            /* enter code generation, and collision fix-up */
            if (0 == (LIBXSMM_HASH_COLLISION & flux_entry.uval)) {
              assert(0 != flux_entry.ptr_const); /* collision */
              mode = 3;
            }
            else
#endif      /* search for an existing code version */
            {
              mode = 1;
            }
          }
          i = LIBXSMM_HASH_MOD(i + 1, LIBXSMM_CAPACITY_REGISTRY);
          if (i == i0) { /* search finished, no code version exists */
#if defined(LIBXSMM_HASH_COLLISION)
            mode = 3; /* enter code generation, and collision fix-up */
#else
            mode = 2; /* enter code generation */
#endif
          }
          assert(0 != diff); /* continue */
        }
      }
      else { /* enter code generation (there is no code version yet) */
        assert(0 == mode || 1 < mode);
#if (0 != LIBXSMM_JIT)
        if (LIBXSMM_X86_AVX <= libxsmm_target_archid) { /* check if JIT is supported (CPUID) */
          assert(0 != mode || 0 == flux_entry.ptr_const/*code version does not exist*/);
          INTERNAL_FIND_CODE_LOCK(lock, i, diff, flux_entry.pmm); /* lock the registry entry */
          if (0 == internal_registry[i].ptr_const) { /* double-check registry after acquiring the lock */
            libxsmm_build_request request; /* setup the code build request */
            request.descriptor.gemm = descriptor;
            if (LIBXSMM_KERNEL_KIND_MCOPY != descriptor->iflags) {
              if (LIBXSMM_KERNEL_KIND_TCOPY != descriptor->iflags) { /* GEMM */
                internal_update_mmstatistic(descriptor, 1/*try*/, 0); /* count attempt */
                request.kind = LIBXSMM_BUILD_KIND_GEMM;
              }
              else { /* transpose */
                request.kind = LIBXSMM_BUILD_KIND_TRANS;
              }
            }
            else { /* matcopy */
              request.kind = LIBXSMM_BUILD_KIND_MCOPY;
            }
            if (EXIT_SUCCESS == libxsmm_build(&request, i, &flux_entry) && 0 != flux_entry.ptr_const) {
              internal_registry_keys[i].xgemm = *descriptor;
# if (0 < INTERNAL_REGLOCK_MAXN)
              LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_STORE, LIBXSMM_BITS)(&internal_registry[i].pmm, flux_entry.pmm, LIBXSMM_ATOMIC_RELAXED);
# else
              internal_registry[i].pmm = flux_entry.pmm;
# endif
# if defined(LIBXSMM_HASH_COLLISION)
              if (2 < mode) { /* arrived from collision state; now mark as collision */
                libxsmm_code_pointer fix_entry;
#   if (0 < INTERNAL_REGLOCK_MAXN)
                fix_entry.pmm = LIBXSMM_ATOMIC_LOAD(&internal_registry[i0].pmm, LIBXSMM_ATOMIC_RELAXED);
#   else
                fix_entry.pmm = internal_registry[i0].pmm;
#   endif
                assert(0 != fix_entry.ptr_const);
                if (0 == (LIBXSMM_HASH_COLLISION & fix_entry.uval)) {
                  fix_entry.uval |= LIBXSMM_HASH_COLLISION; /* mark current entry as collision */
#   if (0 < INTERNAL_REGLOCK_MAXN)
                  LIBXSMM_ATOMIC_STORE(&internal_registry[i0].pmm, fix_entry.pmm, LIBXSMM_ATOMIC_RELAXED);
#   else
                  internal_registry[i0].pmm = fix_entry.pmm;
#   endif
                }
              }
# endif
            }
            diff = 0; /* inside of locked region (do not use break!) */
          }
          INTERNAL_FIND_CODE_UNLOCK(lock);
          if (0 != diff) { /* acquire registry slot */
            if (0 == mode) { /* initial condition */
              mode = 2; /* continue to linearly search for an empty slot */
              i0 = i; /* keep current position on record */
            }
            for (i = LIBXSMM_HASH_MOD(i + 1, LIBXSMM_CAPACITY_REGISTRY); i != i0 && 0 != internal_registry[i].ptr_const;
                 i = LIBXSMM_HASH_MOD(i + 1, LIBXSMM_CAPACITY_REGISTRY)); /* continue to linearly search code */
            if (i == i0) { /* out of capacity (no registry slot available) */
              diff = 0; /* inside of locked region (do not use break!) */
            }
            flux_entry.pmm = 0; /* no result */
          }
        }
        else /* JIT-code generation not available */
#endif
        { /* leave the dispatch loop */
          flux_entry.pmm = 0;
          diff = 0;
        }
      }
    }
#if defined(LIBXSMM_CAPACITY_CACHE) && (0 < (LIBXSMM_CAPACITY_CACHE))
    if (0 != flux_entry.ptr_const) { /* keep code version on record (cache) */
      INTERNAL_FIND_CODE_CACHE_INDEX(cache.hit, cache_index);
      cache.keys[cache_index] = *descriptor;
      cache.code[cache_index] = flux_entry;
      cache.hit = cache_index;
      assert(0 == diff);
    }
    if (cache.id != internal_teardown) {
      memset(cache.keys, 0, sizeof(cache.keys));
      cache.id = internal_teardown;
    }
#endif
#if !defined(NDEBUG)
    refdesc = &internal_registry_keys[i].xgemm;
#endif
  }
  assert(0 == flux_entry.ptr_const || 0 == refdesc || 0 == memcmp(refdesc, descriptor, LIBXSMM_GEMM_DESCRIPTOR_SIZE));
#if defined(LIBXSMM_HASH_COLLISION)
  flux_entry.uval &= ~(LIBXSMM_CODE_STATIC | LIBXSMM_HASH_COLLISION); /* clear non-JIT and collision flag */
#else
  flux_entry.uval &= ~LIBXSMM_CODE_STATIC; /* clear non-JIT flag */
#endif
  return flux_entry;
}


LIBXSMM_API_DEFINITION const libxsmm_kernel_info* libxsmm_get_kernel_info(libxsmm_code_pointer code, libxsmm_kernel_kind* kind, size_t* size)
{
  const libxsmm_kernel_info* result;
  void* extra = 0;
  if (0 != code.ptr_const && 0 != internal_registry && 0 != internal_registry_keys
    && EXIT_SUCCESS == libxsmm_get_malloc_xinfo(code.ptr_const, size, 0/*flags*/, &extra)
    && 0 != extra && *((const unsigned int*)extra) < (LIBXSMM_CAPACITY_REGISTRY)
    && code.ptr_const == internal_registry[*((const unsigned int*)extra)].ptr_const
    /* the kernel kind is stored in the internal flags of the libxsmm_gemm_descriptor (iflags). */
    && internal_registry_keys[*((const unsigned int*)extra)].xgemm.iflags < LIBXSMM_KERNEL_KIND_INVALID)
  {
    if (0 != kind) *kind = (libxsmm_kernel_kind)internal_registry_keys[*((const unsigned int*)extra)].xgemm.iflags;
    result = internal_registry_keys + *((const unsigned int*)extra);
  }
  else {
    if (0 != kind) *kind = LIBXSMM_KERNEL_KIND_INVALID;
    result = 0;
  }
  return result;
}


LIBXSMM_API_DEFINITION int libxsmm_get_kernel_kind(const void* kernel, libxsmm_kernel_kind* kind)
{
  libxsmm_code_pointer code; code.ptr_const = kernel;
  return (0 != libxsmm_get_kernel_info(code, kind, 0/*code_size*/) ? EXIT_SUCCESS : EXIT_FAILURE);
}


LIBXSMM_API_DEFINITION int libxsmm_get_mmkernel_info(libxsmm_xmmfunction kernel, libxsmm_gemm_descriptor* info, size_t* code_size)
{
  libxsmm_code_pointer code;
  libxsmm_kernel_kind kind;
  static int error_once = 0;
  int result;
  code.xgemm = kernel;
  if (0 != info || 0 != code_size) {
    const libxsmm_kernel_info *const kernel_info = libxsmm_get_kernel_info(code, &kind, code_size);
    if (0 != kernel_info && LIBXSMM_KERNEL_KIND_MATMUL == kind) {
      if (0 != info) *info = kernel_info->xgemm;
      result = EXIT_SUCCESS;
    }
    else {
      if (0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: invalid kernel cannot be inspected!\n");
      }
      result = EXIT_FAILURE;
    }
  }
  else {
    if (0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: invalid argument!\n");
    }
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API_DEFINITION int libxsmm_get_transkernel_info(libxsmm_xtransfunction kernel, libxsmm_transpose_descriptor* info, size_t* code_size)
{
  libxsmm_code_pointer code;
  libxsmm_kernel_kind kind;
  static int error_once = 0;
  int result;
  code.xtrans = kernel;
  if (0 != info || 0 != code_size) {
    const libxsmm_kernel_info *const kernel_info = libxsmm_get_kernel_info(code, &kind, code_size);
    if (0 != kernel_info && LIBXSMM_KERNEL_KIND_TCOPY == kind) {
      if (0 != info) *info = kernel_info->trans;
      result = EXIT_SUCCESS;
    }
    else {
      if (0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: invalid kernel cannot be inspected!\n");
      }
      result = EXIT_FAILURE;
    }
  }
  else {
    if (0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: invalid argument!\n");
    }
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API_DEFINITION int libxsmm_get_matcopykernel_info(libxsmm_xmatcopyfunction kernel, libxsmm_matcopy_descriptor* info, size_t* code_size)
{
  libxsmm_code_pointer code;
  libxsmm_kernel_kind kind;
  static int error_once = 0;
  int result;
  code.xmatcopy = kernel;
  if (0 != info || 0 != code_size) {
    const libxsmm_kernel_info *const kernel_info = libxsmm_get_kernel_info(code, &kind, code_size);
    if (0 != kernel_info && LIBXSMM_KERNEL_KIND_MCOPY == kind) {
      if (0 != info) *info = kernel_info->mcopy;
      result = EXIT_SUCCESS;
    }
    else {
      if (0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: invalid kernel cannot be inspected!\n");
      }
      result = EXIT_FAILURE;
    }
  }
  else {
    if (0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: invalid argument!\n");
    }
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API_DEFINITION int libxsmm_get_registry_info(libxsmm_registry_info* info)
{
  int result = EXIT_SUCCESS;
  if (0 != info) {
    LIBXSMM_INIT
    if (0 != internal_registry) {
      size_t i;
      memset(info, 0, sizeof(libxsmm_registry_info)); /* info->nstatic = 0; info->size = 0; */
      info->nbytes = (LIBXSMM_CAPACITY_REGISTRY) * (sizeof(libxsmm_code_pointer) + sizeof(libxsmm_kernel_info));
      info->capacity = LIBXSMM_CAPACITY_REGISTRY;
      info->ncache = LIBXSMM_CAPACITY_CACHE;
      for (i = 0; i < (LIBXSMM_CAPACITY_REGISTRY); ++i) {
        libxsmm_code_pointer code = internal_registry[i];
        if (0 != code.ptr_const && EXIT_SUCCESS == result) {
          if (0 == (LIBXSMM_CODE_STATIC & code.uval)) { /* check for allocated/generated JIT-code */
            size_t buffer_size = 0;
            void* buffer = 0;
#if defined(LIBXSMM_HASH_COLLISION)
            code.uval &= ~LIBXSMM_HASH_COLLISION; /* clear collision flag */
#endif
            result = libxsmm_get_malloc_xinfo(code.ptr_const, &buffer_size, 0/*flags*/, &buffer);
            if (EXIT_SUCCESS == result) {
              info->nbytes += (unsigned int)(buffer_size + (((char*)code.ptr_const) - (char*)buffer));
            }
          }
          else {
            ++info->nstatic;
          }
          ++info->size;
        }
      }
    }
    else {
      result = EXIT_FAILURE;
    }
  }
  else {
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API_DEFINITION int libxsmm_gemm_descriptor_init(libxsmm_gemm_descriptor* descriptor,
  libxsmm_gemm_precision precision, libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const void* alpha, const void* beta, const int* flags, const int* prefetch)
{
  const libxsmm_blasint ilda = (0 == lda ? m : *lda), ildb = (0 == ldb ? k : *ldb), ildc = (0 == ldc ? m : *ldc);
  const int internal_prefetch = (0 == prefetch ? libxsmm_gemm_auto_prefetch : *prefetch);
  const int iflags = (0 == flags ? LIBXSMM_FLAGS : *flags);
  int result;

  switch (precision) {
    case LIBXSMM_GEMM_PRECISION_F64: {
      result = libxsmm_dgemm_descriptor_init(descriptor, m, n, k, ilda, ildb, ildc,
        0 != alpha ? *((const double*)alpha) : (LIBXSMM_ALPHA),
        0 != beta ? *((const double*)beta) : (LIBXSMM_BETA),
        iflags, internal_prefetch);
    } break;
    case LIBXSMM_GEMM_PRECISION_F32: {
      result = libxsmm_sgemm_descriptor_init(descriptor, m, n, k, ilda, ildb, ildc,
        0 != alpha ? *((const float*)alpha) : (LIBXSMM_ALPHA),
        0 != beta ? *((const float*)beta) : (LIBXSMM_BETA),
        iflags, internal_prefetch);
    } break;
    case LIBXSMM_GEMM_PRECISION_I16: {
      /**
       * Take alpha and beta as short data although wgemm works on integers.
       * However, alpha and beta are only JIT-supported for certain values,
       * and the call-side may not distinct different input and output types
       * (integer/short), hence it is safer to only read short data.
       */
      result = libxsmm_wgemm_descriptor_init(descriptor, m, n, k, ilda, ildb, ildc,
        0 != alpha ? *((const short*)alpha) : (LIBXSMM_ALPHA),
        0 != beta ? *((const short*)beta) : (LIBXSMM_BETA),
        iflags, internal_prefetch);
    } break;
    default: {
      static int error_once = 0;
      if (0 != libxsmm_verbosity /* library code is expected to be mute */
       && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: GEMM precision is not supported!\n");
      }
      result = EXIT_FAILURE;
    }
  }

  return result;
}


LIBXSMM_API_DEFINITION libxsmm_xmmfunction libxsmm_xmmdispatch(const libxsmm_gemm_descriptor* descriptor)
{
  libxsmm_xmmfunction result = { 0 };
  /* there is no need to check LIBXSMM_GEMM_NO_BYPASS_DIMS (M, N, K, LDx) since we already got a descriptor */
  if (0 != descriptor && LIBXSMM_GEMM_NO_BYPASS(descriptor->flags, descriptor->alpha, descriptor->beta)) {
    libxsmm_gemm_descriptor backend_descriptor;
    LIBXSMM_INIT
    if (0 > (int)descriptor->prefetch) {
      backend_descriptor = *descriptor;
      LIBXSMM_GEMM_DESCRIPTOR_PREFETCH(backend_descriptor, libxsmm_gemm_auto_prefetch);
      descriptor = &backend_descriptor;
    }
    result = internal_find_code(descriptor).xgemm;
  }
  else { /* bypass (not supported) */
    internal_update_mmstatistic(descriptor, 1/*try*/, 0);
  }
  return result;
}


#if !defined(LIBXSMM_BUILD) && defined(__APPLE__) && defined(__MACH__) && defined(__clang__) && !defined(__INTEL_COMPILER)
LIBXSMM_PRAGMA_OPTIMIZE_OFF
#endif

LIBXSMM_API_DEFINITION libxsmm_dmmfunction libxsmm_dmmdispatch(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const double* alpha, const double* beta,
  const int* flags, const int* prefetch)
{
  LIBXSMM_INIT
  INTERNAL_DISPATCH(double, descriptor, flags, m, n, k, lda, ldb, ldc, alpha, beta, prefetch);
}


LIBXSMM_API_DEFINITION libxsmm_smmfunction libxsmm_smmdispatch(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const float* alpha, const float* beta,
  const int* flags, const int* prefetch)
{
  LIBXSMM_INIT
  INTERNAL_DISPATCH(float, descriptor, flags, m, n, k, lda, ldb, ldc, alpha, beta, prefetch);
}


LIBXSMM_API_DEFINITION libxsmm_wmmfunction libxsmm_wmmdispatch(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const int* alpha, const int* beta,
  const int* flags, const int* prefetch)
{
  LIBXSMM_INIT
  INTERNAL_DISPATCH(short, descriptor, flags, m, n, k, lda, ldb, ldc, alpha, beta, prefetch);
}


#if !defined(LIBXSMM_BUILD) && defined(__APPLE__) && defined(__MACH__) && defined(__clang__) && !defined(__INTEL_COMPILER)
LIBXSMM_PRAGMA_OPTIMIZE_ON
#endif

LIBXSMM_API_DEFINITION libxsmm_xmatcopyfunction libxsmm_xmatcopydispatch(const libxsmm_matcopy_descriptor* descriptor)
{
  libxsmm_xmatcopyfunction result = { 0 };
  if (0 != descriptor) {
    libxsmm_kernel_info query = { { 0 } };
    assert(LIBXSMM_SIZEOF(descriptor, &descriptor->flags) < sizeof(query));
    LIBXSMM_INIT
    query.mcopy = *descriptor;
#if defined(_WIN32) || defined(__CYGWIN__) /* TODO: full support for Windows calling convention */
    query.mcopy.prefetch = 0;
#endif
    query.xgemm.iflags = LIBXSMM_KERNEL_KIND_MCOPY;
    result = internal_find_code(&query.xgemm).xmatcopy;
  }
  return result;
}


LIBXSMM_API_DEFINITION libxsmm_xtransfunction libxsmm_xtransdispatch(const libxsmm_transpose_descriptor* descriptor)
{
  libxsmm_xtransfunction result = { 0 };
  if (0 != descriptor && 0 != LIBXSMM_TRANS_NO_BYPASS_DIMS(descriptor->m, descriptor->n, descriptor->ldo)) {
    libxsmm_kernel_info query = { { 0 } };
    assert(LIBXSMM_SIZEOF(descriptor, &descriptor->typesize) < sizeof(query));
    LIBXSMM_INIT
    query.trans = *descriptor;
    query.xgemm.iflags = LIBXSMM_KERNEL_KIND_TCOPY;
    result = internal_find_code(&query.xgemm).xtrans;
  }
  return result;
}


LIBXSMM_API_DEFINITION libxsmm_xmmfunction libxsmm_create_xcsr_soa(const libxsmm_gemm_descriptor* descriptor,
  const unsigned int* row_ptr, const unsigned int* column_idx, const void* values)
{
  libxsmm_code_pointer result = { 0 };
  if (0 != descriptor && 0 != row_ptr && 0 != column_idx && 0 != values) {
    libxsmm_csr_soa_descriptor srsoa;
    libxsmm_build_request request;
#if defined(_WIN32) || defined(__CYGWIN__) /* TODO: full support for Windows calling convention */
    libxsmm_gemm_descriptor gemm = *descriptor;
    LIBXSMM_GEMM_DESCRIPTOR_PREFETCH(gemm, LIBXSMM_PREFETCH_NONE);
    descriptor = &gemm;
#endif
    LIBXSMM_INIT
    srsoa.gemm = descriptor;
    srsoa.row_ptr = row_ptr;
    srsoa.column_idx = column_idx;
    srsoa.values = values;
    request.descriptor.srsoa = &srsoa;
    request.kind = LIBXSMM_BUILD_KIND_SRSOA;
    libxsmm_build(&request, LIBXSMM_CAPACITY_REGISTRY/*not managed*/, &result);
  }
  return result.xgemm;
}


LIBXSMM_API_DEFINITION libxsmm_xmmfunction libxsmm_create_xcsc_soa(const libxsmm_gemm_descriptor* descriptor,
  const unsigned int* column_ptr, const unsigned int* row_idx, const void* values)
{
  libxsmm_code_pointer result = { 0 };
  if (0 != descriptor && 0 != column_ptr && 0 != row_idx && 0 != values) {
    libxsmm_csc_soa_descriptor scsoa;
    libxsmm_build_request request;
#if defined(_WIN32) || defined(__CYGWIN__) /* TODO: full support for Windows calling convention */
    libxsmm_gemm_descriptor gemm = *descriptor;
    LIBXSMM_GEMM_DESCRIPTOR_PREFETCH(gemm, LIBXSMM_PREFETCH_NONE);
    descriptor = &gemm;
#endif
    LIBXSMM_INIT
    scsoa.gemm = descriptor;
    scsoa.column_ptr = column_ptr;
    scsoa.row_idx = row_idx;
    scsoa.values = values;
    request.descriptor.scsoa = &scsoa;
    request.kind = LIBXSMM_BUILD_KIND_SCSOA;
    libxsmm_build(&request, LIBXSMM_CAPACITY_REGISTRY/*not managed*/, &result);
  }
  return result.xgemm;
}


LIBXSMM_API_DEFINITION libxsmm_dmmfunction libxsmm_create_dcsr_reg(const libxsmm_gemm_descriptor* descriptor,
  const unsigned int* row_ptr, const unsigned int* column_idx, const double* values)
{
  libxsmm_code_pointer result = { 0 };
  if (0 != descriptor && 0 != row_ptr && 0 != column_idx && 0 != values) {
    libxsmm_csr_reg_descriptor sreg;
    libxsmm_build_request request;
#if defined(_WIN32) || defined(__CYGWIN__) /* TODO: full support for Windows calling convention */
    libxsmm_gemm_descriptor gemm = *descriptor;
    LIBXSMM_GEMM_DESCRIPTOR_PREFETCH(gemm, LIBXSMM_PREFETCH_NONE);
    descriptor = &gemm;
#endif
    LIBXSMM_INIT
    sreg.gemm = descriptor;
    sreg.row_ptr = row_ptr;
    sreg.column_idx = column_idx;
    sreg.values = values;
    request.descriptor.sreg = &sreg;
    request.kind = LIBXSMM_BUILD_KIND_SREG;
    libxsmm_build(&request, LIBXSMM_CAPACITY_REGISTRY/*not managed*/, &result);
  }
  return result.xgemm.dmm;
}


LIBXSMM_API_DEFINITION libxsmm_smmfunction libxsmm_create_scsr_reg(const libxsmm_gemm_descriptor* descriptor,
  const unsigned int* row_ptr, const unsigned int* column_idx, const float* values)
{
  libxsmm_code_pointer result = { 0 };
  if (0 != descriptor && 0 != row_ptr && 0 != column_idx && 0 != values) {
    libxsmm_csr_reg_descriptor sreg;
    libxsmm_build_request request;
    const unsigned int n = row_ptr[descriptor->m];
    double *const d_values = (double*)malloc(n * sizeof(double));
#if defined(_WIN32) || defined(__CYGWIN__) /* TODO: full support for Windows calling convention */
    libxsmm_gemm_descriptor gemm = *descriptor;
    LIBXSMM_GEMM_DESCRIPTOR_PREFETCH(gemm, LIBXSMM_PREFETCH_NONE);
    descriptor = &gemm;
#endif
    if (0 != d_values) {
      unsigned int i;
      LIBXSMM_INIT
      /* we need to copy the values into a double precision buffer */
      for (i = 0; i < n; ++i) d_values[i] = (double)values[i];
      sreg.gemm = descriptor;
      sreg.row_ptr = row_ptr;
      sreg.column_idx = column_idx;
      sreg.values = d_values;
      request.descriptor.sreg = &sreg;
      request.kind = LIBXSMM_BUILD_KIND_SREG;
      libxsmm_build(&request, LIBXSMM_CAPACITY_REGISTRY/*not managed*/, &result);
      free(d_values);
    }
  }
  return result.xgemm.smm;
}


LIBXSMM_API_DEFINITION void libxsmm_release_kernel(const void* jit_code)
{
  void* extra = 0;
  LIBXSMM_INIT
  if (EXIT_SUCCESS == libxsmm_get_malloc_xinfo(jit_code, 0/*size*/, 0/*flags*/, &extra) && 0 != extra) {
    const unsigned int regindex = *((const unsigned int*)extra);
    if ((LIBXSMM_CAPACITY_REGISTRY) <= regindex) {
      libxsmm_xfree(jit_code);
    }
#if !defined(NDEBUG)
    else { /* TODO: implement to unregister GEMM kernels */
      fprintf(stderr, "LIBXSMM WARNING: attempt to unregister a JIT-kernel!\n");
    }
#endif
  }
  else if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
    static int error_once = 0;
    if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) {
      fprintf(stderr, "LIBXSMM ERROR: failed to release kernel!\n");
    }
  }
}


LIBXSMM_API_DEFINITION int libxsmm_matdiff(libxsmm_datatype datatype, libxsmm_blasint m, libxsmm_blasint n,
  const void* ref, const void* tst, const libxsmm_blasint* ldref, const libxsmm_blasint* ldtst,
  libxsmm_matdiff_info* info)
{
  int result = EXIT_SUCCESS;
  if (0 != ref && 0 != tst && 0 != info) {
    libxsmm_blasint mm = m, nn = n, ldr = (0 == ldref ? m : *ldref), ldt = (0 == ldtst ? m : *ldtst);
    if (1 == n) { mm = ldr = ldt = 1; nn = m; } /* ensure row-vector shape to standardize results */
    memset(info, 0, sizeof(*info)); /* nullify */
    switch(datatype) {
      case LIBXSMM_DATATYPE_F64: {
#       define LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE double
#       include "template/libxsmm_matdiff.tpl.c"
#       undef  LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE
      } break;
      case LIBXSMM_DATATYPE_F32: {
#       define LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE float
#       include "template/libxsmm_matdiff.tpl.c"
#       undef  LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE
      } break;
      case LIBXSMM_DATATYPE_I32: {
#       define LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE int
#       include "template/libxsmm_matdiff.tpl.c"
#       undef  LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE
      } break;
      case LIBXSMM_DATATYPE_I16: {
#       define LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE short
#       include "template/libxsmm_matdiff.tpl.c"
#       undef  LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE
      } break;
      case LIBXSMM_DATATYPE_I8: {
#       define LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE signed char
#       include "template/libxsmm_matdiff.tpl.c"
#       undef  LIBXSMM_MATDIFF_TEMPLATE_ELEM_TYPE
      } break;
      default: {
        static int error_once = 0;
        if (0 != libxsmm_verbosity /* library code is expected to be mute */
         && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
        {
          fprintf(stderr, "LIBXSMM ERROR: unsupported data-type requested for libxsmm_matdiff!\n");
        }
        result = EXIT_FAILURE;
      }
    }
  }
  else {
    result = EXIT_FAILURE;
  }
  if (EXIT_SUCCESS == result) { /* square-root without libm dependency */
    int i;
    if (0 < info->l2_abs) {
      const double squared = info->l2_abs; info->l2_abs *= 0.5;
      for (i = 0; i < 16; ++i) info->l2_abs = 0.5 * (info->l2_abs + squared / info->l2_abs);
    }
    if (0 < info->l2_rel) {
      const double squared = info->l2_rel; info->l2_rel *= 0.5;
      for (i = 0; i < 16; ++i) info->l2_rel = 0.5 * (info->l2_rel + squared / info->l2_rel);
    }
    if (0 < info->normf_rel) {
      const double squared = info->normf_rel; info->normf_rel *= 0.5;
      for (i = 0; i < 16; ++i) info->normf_rel = 0.5 * (info->normf_rel + squared / info->normf_rel);
    }
    if (1 == n) {
      const libxsmm_blasint tmp = info->linf_abs_m;
      info->linf_abs_m = info->linf_abs_n;
      info->linf_abs_n = tmp;
    }
  }
  return result;
}


#if defined(LIBXSMM_BUILD)

/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_init)(void);
LIBXSMM_API_DEFINITION void LIBXSMM_FSYMBOL(libxsmm_init)(void)
{
  libxsmm_init();
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_finalize)(void);
LIBXSMM_API_DEFINITION void LIBXSMM_FSYMBOL(libxsmm_finalize)(void)
{
  libxsmm_finalize();
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_xmmdispatch)(intptr_t* fn,
  const libxsmm_gemm_precision* precision, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const void* alpha, const void* beta, const int* flags, const int* prefetch);
LIBXSMM_API_DEFINITION void LIBXSMM_FSYMBOL(libxsmm_xmmdispatch)(intptr_t* fn,
  const libxsmm_gemm_precision* precision, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const void* alpha, const void* beta, const int* flags, const int* prefetch)
{
#if !defined(NDEBUG) /* this should not happen */
  static int error_once = 0;
  if (0 != fn && 0 != m)
#endif
  {
    const libxsmm_gemm_precision gemm_precision = (0 != precision ? *precision : LIBXSMM_GEMM_PRECISION_F64);
    const libxsmm_blasint kk = *(0 != k ? k : m), nn = (0 != n ? *n : kk);
    switch (gemm_precision) {
      case LIBXSMM_GEMM_PRECISION_F64: {
        *fn = (intptr_t)libxsmm_dmmdispatch(*m, nn, kk, lda, ldb, ldc,
          (const double*)alpha, (const double*)beta,
          flags, prefetch);
      } break;
      case LIBXSMM_GEMM_PRECISION_F32: {
        *fn = (intptr_t)libxsmm_smmdispatch(*m, nn, kk, lda, ldb, ldc,
          (const float*)alpha, (const float*)beta,
          flags, prefetch);
      } break;
      case LIBXSMM_GEMM_PRECISION_I16: {
        *fn = (intptr_t)libxsmm_wmmdispatch(*m, nn, kk, lda, ldb, ldc,
          (const int*)alpha, (const int*)beta,
          flags, prefetch);
      } break;
#if !defined(NDEBUG) /* this should not happen */
      default: {
        if (0 != libxsmm_verbosity /* library code is expected to be mute */
         && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
        {
          fprintf(stderr, "LIBXSMM ERROR: invalid precision requested for libxsmm_xmmdispatch!\n");
        }
        *fn = 0;
      }
#endif
    }
  }
#if !defined(NDEBUG)
  else {
    if (0 != libxsmm_verbosity /* library code is expected to be mute */
     && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: invalid M, N, or K passed into libxsmm_xmmdispatch!\n");
    }
    if (0 != fn) *fn = 0;
  }
#endif
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_xmmcall_abc)(
  const libxsmm_code_pointer* fn, const void* a, const void* b, void* c);
LIBXSMM_API_DEFINITION void LIBXSMM_FSYMBOL(libxsmm_xmmcall_abc)(
  const libxsmm_code_pointer* fn, const void* a, const void* b, void* c)
{
#if !defined(NDEBUG) /* this should not happen */
  static int error_once = 0;
  if (0 != fn && 0 != a && 0 != b && 0 != c)
#endif
  {
#if !defined(NDEBUG) /* this should not happen */
    if (0 != fn->xgemm.xmm)
#endif
    {
      fn->xgemm.xmm(a, b, c);
    }
#if !defined(NDEBUG)
    else if (0 != libxsmm_verbosity /* library code is expected to be mute */
          && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: NULL-function passed into libxsmm_xmmcall_abc!\n");
    }
#endif
  }
#if !defined(NDEBUG)
  else if (0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
  {
    fprintf(stderr, "LIBXSMM ERROR: invalid arguments for libxsmm_xmmcall_abc specified!\n");
  }
#endif
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_xmmcall_prf)(
  const libxsmm_code_pointer* fn, const void* a, const void* b, void* c,
  const void* pa, const void* pb, const void* pc);
LIBXSMM_API_DEFINITION void LIBXSMM_FSYMBOL(libxsmm_xmmcall_prf)(
  const libxsmm_code_pointer* fn, const void* a, const void* b, void* c,
  const void* pa, const void* pb, const void* pc)
{
#if !defined(NDEBUG) /* this should not happen */
  static int error_once = 0;
  if (0 != fn && 0 != a && 0 != b && 0 != c)
#endif
  {
#if !defined(NDEBUG) /* this should not happen */
    if (0 != fn->xgemm.xmm)
#endif
    {
      fn->xgemm.xmm(a, b, c, pa, pb, pc);
    }
#if !defined(NDEBUG)
    else if (0 != libxsmm_verbosity /* library code is expected to be mute */
          && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: NULL-function passed into libxsmm_xmmcall_prf!\n");
    }
#endif
  }
#if !defined(NDEBUG)
  else if (0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
  {
    fprintf(stderr, "LIBXSMM ERROR: invalid arguments for libxsmm_xmmcall_prf specified!\n");
  }
#endif
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_xmmcall)(
  const libxsmm_code_pointer* fn, const void* a, const void* b, void* c,
  const void* pa, const void* pb, const void* pc);
LIBXSMM_API_DEFINITION void LIBXSMM_FSYMBOL(libxsmm_xmmcall)(
  const libxsmm_code_pointer* fn, const void* a, const void* b, void* c,
  const void* pa, const void* pb, const void* pc)
{
  LIBXSMM_FSYMBOL(libxsmm_xmmcall_prf)(fn, a, b, c, pa, pb, pc);
}

#endif /*defined(LIBXSMM_BUILD)*/

