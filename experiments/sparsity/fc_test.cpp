/*
 * conv1_test.cpp
 *
 *  Created on: Apr 15, 2016
 *      Author: jpark103
 */

#include <cstdio>
#include <omp.h>
#include <immintrin.h>
#include <vector>
#include <cassert>
#include <cstring>
#include <cmath>

#define CSRMM_REARRANGE_B
#include "../../include/caffe/util/csrmm.hpp"
#include "SpMP/CSR.hpp"
#include "SpMP/synk/barrier.hpp"
#include <mkl.h>

#ifdef SEP
#include "sampling.h"
#endif

#ifdef VTUNE
#include "ittnotify.h"
#endif

#ifdef SNIPER
#include "sim_api.h"
#endif

#ifdef SDE
#include "../../include/caffe/util/sde.h"
#endif

using namespace std;

synk::Barrier *barriers[256];

unsigned long long conv_cycles_of_this_batch[1024*16];
unsigned long long reduce_cycles[1024*16];
int flop_cnt = 0;

int main(int argc, const char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "Usage: %s matrix_in_matrix_market_format [N default=256] [bk default=256]\n", argv[0]);
    return -1;
  }

  int nbatch = argc > 2 ? atoi(argv[2]) : 256;

  int nthreads = omp_get_max_threads();

  double cpu_freq = SpMP::get_cpu_freq();
  printf("freq = %g\n", cpu_freq);

  SpMP::CSR *A = new SpMP::CSR(argv[1]);
  printf("m = %d n = %d k = %d nnz_proportion = %g\n", A->m, nbatch, A->n, (double)A->getNnz()/A->m/A->n);

#if defined(SNIPER) || defined(SDE)
  int NOUT = A->m/32; // scale down to 1 tile
#elif defined(SDE)
  int NOUT = A->m/64; // scale down to 1 core
#else
  int NOUT = A->m;
#endif
  int NIN = A->n;

  // C decomposition
  typedef int idx_t;

  int bk = argc > 3 ? atoi(argv[3]) : 256;
  int kb = (NIN + bk - 1)/bk;

  int bn = VLEN*CSRMM_REG_BLOCK_SIZE;
  int nb = (nbatch + bn - 1)/bn;
    // C col block size -> AVX512: 256/(16*4) = 4, AVX2: 256/(8*8) = 4, SSE: 64/(4*8) = 2
  if (nthreads < nb) {
    bn = (nbatch + nthreads - 1)/nthreads;
    nb = (nbatch + bn - 1)/bn;
  }
  if (nthreads%nb != 0) {
    fprintf(stderr, "nb %d should divide # of threads %d\n", nb, nthreads);
    return -1;
  }
  int num_of_C_row_partitions = nthreads/nb;

  int mb = num_of_C_row_partitions;
  printf("mb = %d, nb = %d, kb = %d\n", num_of_C_row_partitions, nb, kb);

  int nnz = A->getNnz();

  // 2D blocking of weight matrix A
  int *weight_i_blocked_;
  idx_t *weight_j_blocked_;
  float *weight_values_blocked_;

  posix_memalign((void **)&weight_i_blocked_, 4096, sizeof(int)*(NOUT*kb + 1));
  posix_memalign((void **)&weight_j_blocked_, 4096, sizeof(idx_t)*nnz);
  posix_memalign((void **)&weight_values_blocked_, 4096, sizeof(float)*nnz);

  weight_i_blocked_[0] = 0;
  nnz = 0;
  int i_per_row_block = (NOUT + mb - 1)/mb;
  int j_per_col_block = (NIN + kb - 1)/kb;

  for (int row_block = 0; row_block < mb; ++row_block) {
    int i_begin = std::min(i_per_row_block*row_block, NOUT);
    int i_end = std::min(i_begin + i_per_row_block, NOUT);

    for (int col_block = 0; col_block < kb; ++col_block) {
      int c_begin = std::min(j_per_col_block*col_block, NIN);
      int c_end = std::min(c_begin + j_per_col_block, NIN);

      for (int i = i_begin; i < i_end; ++i) {
        for (int j = A->rowptr[i]; j < A->rowptr[i + 1]; ++j) {
          int c = A->colidx[j];
          if (c >= c_begin && c < c_end) {
            if (sizeof(idx_t) == 2) {
              weight_j_blocked_[nnz] = c;
            }
            else {
              // When we have enough bits for column indices,
              // we pre-multiply it with # of columns of matrix B
#ifdef CSRMM_REARRANGE_B
              weight_j_blocked_[nnz] = nbatch/nb*c;
#else
              weight_j_blocked_[nnz] = nbatch*c;
#endif
            }
            weight_values_blocked_[nnz] = A->values[j];
            ++nnz;
          }
        }
        int rowptr_idx = kb*i_begin + col_block*(i_end - i_begin) + (i - i_begin) + 1;
        assert(rowptr_idx <= NOUT*kb);
        weight_i_blocked_[rowptr_idx] = nnz;
      }
    } // for each col block
  } // for each row block
  assert(nnz == A->getNnz());
  assert(weight_i_blocked_[NOUT*kb] == nnz);

  size_t input_size = sizeof(float)*NIN*nbatch;
  float *input = (float *)_mm_malloc(input_size, 4096);
//  float *input = (float *)malloc_huge_pages(sizeof(float)*nbatch*NOUT*(WIDTH + PAD)*(WIDTH + PAD));
  for (int i = 0; i < input_size/sizeof(float); ++i) {
    input[i] = i%123;
  }

  // rearrange input
  float *input_rearranged = (float *)_mm_malloc(input_size*num_of_C_row_partitions, 4096);
  int col_block_size = (nbatch + nb - 1)/nb;
  for (int col_block = 0; col_block < nb; ++col_block) {
    for (int i = 0; i < NIN; ++i) {
      for (int j = 0; j < col_block_size; ++j) {
#ifdef CSRMM_REPLICATE_B
        for (int row_block = 0; row_block < num_of_C_row_partitions; ++row_block) {
          input_rearranged[((col_block*num_of_C_row_partitions + row_block)*NIN + i)*col_block_size + j] =
              input[i*nbatch + col_block*col_block_size + j];
        }
#else
        input_rearranged[(col_block*NIN + i)*col_block_size + j] =
            input[i*nbatch + col_block*col_block_size + j];
#endif
      }
    }
  }

  size_t output_size = sizeof(float)*nthreads*((NOUT + num_of_C_row_partitions - 1)/num_of_C_row_partitions)*(nbatch + nb - 1)/nb;
  float *output = (float *)_mm_malloc(output_size, 4096);
//  float *output = (float *)malloc_huge_pages(sizeof(float)*nbatch*NOUT*(WIDTH + PAD)*(WIDTH + PAD));
  memset((void *)output, 0, output_size);

  float *output_scratch = (float *)_mm_malloc(output_size*nthreads, 4096);
  memset((void *)output_scratch, 0, output_size*nthreads);

  float *bias = (float *)_mm_malloc(sizeof(float)*NOUT, 4096);
//  float *bias = (float *)malloc_huge_pages(sizeof(float)*NOUT);
  for (int i = 0; i < NOUT; ++i) {
    bias[i] = -(i%123);
  }

//  printf(
//      "input = %p, weight_j_blocked_ = %p, weight_values_blocked_ = %p, output = %p, weight_i_blocked = %p\n",
//      input, weight_j_blocked_, weight_values_blocked_, output, weight_i_blocked_);

  unsigned long long times[nthreads*16];
  for (int tid = 0; tid < nthreads; ++tid) {
    times[tid*16] = 0;
    conv_cycles_of_this_batch[tid*16] = 0;
    reduce_cycles[tid*16] = 0;
  }

#if defined(SNIPER) || defined(SDE)
  const int REPEAT = 2;
#else
#ifdef VECTORIZE_OVER_INPUTS
  const int REPEAT = 128;
#else
  const int REPEAT = 256;
#endif
#endif

  printf("REPEAT = %d, nbatch = %d\n", REPEAT, nbatch);

#ifdef SEP
  VTResumeSampling();
#endif
#ifdef VTUNE
  fprintf(stderr, "__itt_resume\n");
  __itt_resume();
#endif
#ifdef SNIPER
  SimWarmup();
#endif
#ifdef SDE
  ssc_initialization();
#endif

  double t = omp_get_wtime();

  for (int j = 0; j < REPEAT; ++j) {
    if (j == REPEAT - 1) {
#ifdef SNIPER
      SimRoiStart();
#endif
#ifdef SDE
      ssc_start_performance();
#endif
    }

    // 2D decomposition of C
    csrmm_fused_C_decomposed(
        weight_values_blocked_, weight_j_blocked_, weight_i_blocked_,
#ifdef CSRMM_REARRANGE_B
        input_rearranged,
#else
        input,
#endif
        output,
        NOUT, nbatch, NIN,
        bias,
        nb,
        kb);

    if (j == REPEAT - 1) {
#ifdef SNIPER
      SimRoiEnd();
#endif
#ifdef SDE
      ssc_stop_performance();
#endif
    }
  }

#ifdef SEP
  VTPauseSampling();
#endif
#ifdef VTUNE
  __itt_pause();
  fprintf(stderr, "__itt_pause\n");
#endif
#ifdef SDE
  ssc_stop_simulation();
#endif

  t = omp_get_wtime() - t;

  // Re-arrange output matrix C
  // In csrmm_fused_C_decomposed, each thread writes to the contiguous locations for spatial locality
  // which is not necessarily match with the original layout of output matrix C.
  float *temp_output = new float[NOUT*nbatch];
  int i_per_block = (NOUT + num_of_C_row_partitions - 1)/num_of_C_row_partitions;
  int j_per_block = (nbatch + nb - 1)/nb;
#pragma omp parallel for
  for (int row_block = 0; row_block < num_of_C_row_partitions; ++row_block) {
    int i_begin = std::min(i_per_block*row_block, NOUT);
    int i_end = std::min(i_begin + i_per_block, NOUT);

    for (int col_block = 0; col_block < nb; ++col_block) {
      int j_begin = std::min(j_per_block*col_block, nbatch);
      int j_end = std::min(j_begin + j_per_block, nbatch);

      for (int i = i_begin; i < i_end; ++i) {
        for (int j = j_begin; j < j_end; ++j) {
          temp_output[i*nbatch + j] =
              output[((row_block*nb + col_block)*i_per_block + i - i_begin)*j_per_block + j - j_begin];
        }
      }
    }
  }
  memcpy(output, temp_output, sizeof(float)*NOUT*nbatch);
  delete[] temp_output;

  unsigned long long max_csrmm_cycles = 0, sum_csrmm_cycles = 0;
  unsigned long long max_reduce_cycles = 0, sum_reduce_cycles = 0;
  for (int tid = 0; tid < nthreads; ++tid) {
    max_csrmm_cycles = std::max(max_csrmm_cycles, conv_cycles_of_this_batch[tid*16]);
    sum_csrmm_cycles += conv_cycles_of_this_batch[tid*16];

    max_reduce_cycles = std::max(max_reduce_cycles, reduce_cycles[tid*16]);
    sum_reduce_cycles += reduce_cycles[tid*16];
  }

  // correctness check
  const char *matdescra = "GXXCX";
  const char transa = 'N';
  float alpha = 1;
  float beta = 0;
  float *temp_values = new float[A->getNnz()];
  for (int i = 0; i < A->getNnz(); ++i) {
    temp_values[i] = A->values[i];
  }
  float *output_ref = new float[output_size/sizeof(float)];
  mkl_scsrmm(&transa, &NOUT, &nbatch, &NIN, &alpha, matdescra, temp_values, A->colidx, A->rowptr, A->rowptr + 1, input, &nbatch, &beta, output_ref, &nbatch);
  for (int i = 0; i < NOUT; ++i) {
    for (int j = 0; j < nbatch; ++j) {
      output_ref[i*nbatch + j] = std::max<float>(output_ref[i*nbatch + j] + bias[i], 0);
    }
  }

#ifdef DBG_CSRMM
  printf("%g ", bias[ROW_TO_DEBUG]);
  float sum = bias[ROW_TO_DEBUG];
  for (int j = A->rowptr[ROW_TO_DEBUG]; j < A->rowptr[ROW_TO_DEBUG + 1]; ++j) {
    float w = temp_values[j];
    int off = A->colidx[j];
    printf(" + %g*%d:%g", w, off, input[off*nbatch + COL_TO_DEBUG]);
    sum += w*input[off*nbatch + COL_TO_DEBUG];
  }
  printf(" = %g\n", sum);
#endif

  for (int i = 0; i < NOUT; ++i) {
    for (int j = 0; j < nbatch; ++j) {
      float expected = output_ref[i*nbatch + j];
      float actual = output[i*nbatch + j];
      if (fabs(expected - actual)/fabs(expected) > 1e-3) {
        printf("(%d, %d) expected %g actual %g\n", i, j, expected, actual);
        return -1;
      }
    }
  }

  delete[] temp_values;
  delete[] output_ref;

  double flops = (double)NOUT*NIN*2;
  printf("mflops-per-file %g\n", flops/1e6);
  printf("effective-GF/s %g %g\n", flops*REPEAT*nbatch/t/1e9, flops*nbatch/(max_csrmm_cycles/cpu_freq)/1e6);
  printf("wall_clock_time = %g, max_csrmm_time = %g, avg_csrmm_time = %g, max_reduce_time = %g, avx_reduce_time = %g\n", t/REPEAT, max_csrmm_cycles/cpu_freq, (double)sum_csrmm_cycles/nthreads/cpu_freq, max_reduce_cycles/cpu_freq, (double)sum_reduce_cycles/nthreads/cpu_freq);

  return 0;
}
