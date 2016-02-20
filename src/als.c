
#include "completion.h"
#include "csf.h"

#include <math.h>
#include <omp.h>




/******************************************************************************
 * PRIVATE FUNCTIONS
 *****************************************************************************/

/**
* @brief Compute the Cholesky decomposition of the normal equations and solve
*        for out_row.
*
* @param neqs The NxN normal equations.
* @param[out] out_row The RHS of the equation. Updated in place.
* @param N The rank of the problem.
*/
static inline void p_invert_row(
    val_t * const restrict neqs,
    val_t * const restrict out_row,
    idx_t const N)
{
  char uplo = 'U';
  int order = (int) N;
  int lda = (int) N;
  int info;
  dpotrf_(&uplo, &order, neqs, &lda, &info);
  if(info) {
    fprintf(stderr, "SPLATT: DPOTRF returned %d\n", info);
  }

  int nrhs = 1;
  int ldb = (int) N;
  dpotrs_(&uplo, &order, &nrhs, neqs, &lda, out_row, &ldb, &info);
  if(info) {
    fprintf(stderr, "SPLATT: DPOTRF returned %d\n", info);
  }
}



/**
* @brief Compute out += inrow' * inrow, a rank-1 update.
*
* @param inrow The input row to update with.
* @param N The length of 'inrow'.
* @param[out] out The NxN matrix to update.
*/
static inline void p_onevec_oprod(
		val_t const * const restrict inrow,
    idx_t const N,
    val_t * const restrict out)
{
  for(idx_t i=0; i < N; ++i) {
    val_t * const restrict orow = out + (i * N);
    val_t const ival = inrow[i];
    for(idx_t j=0; j < N; ++j) {
      orow[j] += ival * inrow[j];
    }
  }
}




static inline void p_update_row(
    splatt_csf const * const csf,
    idx_t const i,
    idx_t const mode,
    tc_model * const model,
    tc_ws * const ws,
    int const tid)
{
  idx_t const nfactors = model->rank;
  csf_sparsity const * const pt = csf->pt;

  assert(model->nmodes == 3);

  /* fid is the row we are actually updating */
  idx_t const fid = (pt->fids[0] == NULL) ? i : pt->fids[0][i];
  val_t * const restrict out_row = model->factors[csf->dim_perm[0]] +
      (fid * nfactors);
  val_t * const restrict hada  = ws->thds[tid].scratch[0];
  val_t * const restrict accum = ws->thds[tid].scratch[1];
  val_t * const restrict neqs  = ws->thds[tid].scratch[2];

  for(idx_t f=0; f < nfactors; ++f) {
    out_row[f] = 0;
  }

  /* initialize normal eqs */
  for(idx_t f=0; f < nfactors * nfactors; ++f) {
    neqs[f] = 0;
  }

  idx_t const * const restrict sptr = pt->fptr[0];
  idx_t const * const restrict fptr = pt->fptr[1];
  idx_t const * const restrict fids = pt->fids[1];
  idx_t const * const restrict inds = pt->fids[2];

  val_t const * const restrict avals = model->factors[csf->dim_perm[1]];
  val_t const * const restrict bvals = model->factors[csf->dim_perm[2]];
  val_t const * const restrict vals = pt->vals;

  /* process each fiber */
  for(idx_t fib=sptr[i]; fib < sptr[i+1]; ++fib) {
    val_t const * const restrict av = avals  + (fids[fib] * nfactors);

    /* first entry of the fiber is used to initialize accum */
    idx_t const jjfirst  = fptr[fib];
    val_t const vfirst   = vals[jjfirst];
    val_t const * const restrict bv = bvals + (inds[jjfirst] * nfactors);
    for(idx_t r=0; r < nfactors; ++r) {
      accum[r] = vfirst * bv[r];
      hada[r] = av[r] * bv[r];
    }

    /* add to normal equations */
    p_onevec_oprod(hada, nfactors, neqs);

    /* foreach nnz in fiber */
    for(idx_t jj=fptr[fib]+1; jj < fptr[fib+1]; ++jj) {
      val_t const v = vals[jj];
      val_t const * const restrict bv = bvals + (inds[jj] * nfactors);
      for(idx_t r=0; r < nfactors; ++r) {
        accum[r] += v * bv[r];
        hada[r] = av[r] * bv[r];
      }

      /* add to normal equations */
      p_onevec_oprod(hada, nfactors, neqs);
    }

    /* accumulate into output row */
    for(idx_t r=0; r < nfactors; ++r) {
      out_row[r] += accum[r] * av[r];
    }
  }

  /* add regularization to the diagonal */
  val_t const reg = ws->regularization[mode];
  for(idx_t f=0; f < nfactors; ++f) {
    neqs[f + (f * nfactors)] += reg;
  }

  /* solve! */
  p_invert_row(neqs, out_row, nfactors);
}




/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/


void splatt_tc_als(
    sptensor_t * train,
    sptensor_t const * const validate,
    tc_model * const model,
    tc_ws * const ws)
{
  /* convert training data to CSF-ALLMODE */
  double * opts = splatt_default_opts();
  opts[SPLATT_OPTION_CSF_ALLOC] = SPLATT_CSF_ALLMODE;
  opts[SPLATT_OPTION_TILE] = SPLATT_NOTILE;
  splatt_csf * csf = csf_alloc(train, opts);
  assert(csf->ntiles == 1);


  val_t prev_val_rmse = 0;

  sp_timer_t train_time;
  sp_timer_t test_time;
  timer_reset(&train_time);
  timer_reset(&test_time);

  for(idx_t e=0; e < ws->max_its; ++e) {
    timer_start(&train_time);
    #pragma omp parallel
    {
      int const tid = omp_get_thread_num();

      for(idx_t m=0; m < train->nmodes; ++m) {

        idx_t const nslices = csf[m].pt[0].nfibs[0];

        /* update each row in parallel */
        /* TODO: use CCP to statically schedule */
        #pragma omp for schedule(dynamic, 4)
        for(idx_t i=0; i < nslices; ++i) {
          p_update_row(csf+m, i, m, model, ws, tid);
        }
      }

    } /* end omp parallel */

    /* compute RMSE */
    timer_stop(&train_time);
    timer_start(&test_time);
    val_t const loss = tc_loss_sq(train, model, ws);
    val_t const frobsq = tc_frob_sq(model, ws);
    val_t const obj = loss + frobsq;
    val_t const train_rmse = sqrt(loss / train->nnz);
    val_t const val_rmse = tc_rmse(validate, model, ws);
    timer_stop(&test_time);

    printf("epoch:%4"SPLATT_PF_IDX"   obj: %0.5e   "
        "RMSE-tr: %0.5e   RMSE-vl: %0.5e time-tr: %0.3fs  time-ts: %0.3fs\n",
        e+1, obj, train_rmse, val_rmse, train_time.seconds, test_time.seconds);

    if(e > 0) {
      /* check convergence */
      if(fabs(val_rmse - prev_val_rmse) < 1e-8) {
        break;
      }
    }
    prev_val_rmse = val_rmse;

  } /* foreach iteration */
  csf_free(csf, opts);
}



