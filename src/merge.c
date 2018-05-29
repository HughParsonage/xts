/*
#   xts: eXtensible time-series 
#
#   Copyright (C) 2008  Jeffrey A. Ryan jeff.a.ryan @ gmail.com
#
#   Contributions from Joshua M. Ulrich
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 2 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <R.h>
#include <Rinternals.h>
#include <Rdefines.h>
#include <stdint.h>
#include "xts.h"

/*
 * These types and functions allow us to perform a merge on either double-
 * or integer-indexed xts objects without repeating the code for the
 * generic algorithm.
 */

/* A single xts index, either double, int32, or int64 */
typedef union xts_index {
  double *d;
  int64_t *l;
  int32_t *i;
} xts_index;

/* The two xts indices involved in the merge */
typedef struct xts_indices {
  xts_index x;
  xts_index y;
} xts_indices;

typedef struct xts_node {
  int beg;
  int num;
  int out;
} xts_node;

/* dynamic array */
typedef struct xts_node_darray {
  size_t count;
  xts_node values[];
} xts_node_darray;

int darray_insert(xts_node_darray **xnda, xts_node node) {
  size_t x = *xnda ? xnda[0]->count : 0;
  size_t y = (x == 0) ? 5 : x + 1;
Rprintf("%d %d\n", x, y);
  if ((x & y) == 0) {
    int new_size = sizeof **xnda + (x + y) * sizeof xnda[0]->values[0];
    xts_node_darray *temp = (xts_node_darray *)R_alloc(y, new_size);
    if (!temp) {
      return 0;
    }
    //if (x != 0)
Rprintf("%d %d %d %d\n", xnda[0]->count, x, y, x * sizeof (*xnda)->values[0]);
    memcpy((*xnda)->values, temp->values, x * sizeof (*xnda)->values[0]);
    *xnda = temp;
  }

  xnda[0]->values[x] = node;
  xnda[0]->count = y;
  return 1;
}

/* Functions to compare the indices for x and y */
typedef int (*compare_func)(xts_indices *, int, int);
int compare_indexes_double(xts_indices *idx, int xp, int yp) {
  if (idx->x.d[xp] > idx->y.d[yp]) return  1;
  if (idx->x.d[xp] < idx->y.d[yp]) return -1;
  return 0;
}
int compare_indexes_int(xts_indices *idx, int xp, int yp) {
  if (idx->x.i[xp] > idx->y.i[yp]) return  1;
  if (idx->x.i[xp] < idx->y.i[yp]) return -1;
  return 0;
}
int compare_indexes_long(xts_indices *idx, int xp, int yp) {
  if (idx->x.l[xp] > idx->y.l[yp]) return  1;
  if (idx->x.l[xp] < idx->y.l[yp]) return -1;
  return 0;
}

// FIXME: use dynamic arrays: https://stackoverflow.com/a/46351960

struct xts_node_darray {
   size_t count;
   size_t nleft;
   xts_node *values;
};

static int
insert(struct xts_node_darray **darray, xts_node value)
{
  if (!*darray) {
    // initialize
    struct xts_node_darray *temp = NULL;
    temp->values = (xts_node *)R_alloc(sizeof(xts_node), 1024);
    temp->nleft = 1023;
    darray[0] = temp;
    return 1;
  } else if (!(*darray)->nleft) {
    // reallocate
    size_t n = (*darray)->count;
    int node_size = sizeof(xts_node);
    xts_node *new_nodes = (xts_node *)R_alloc(node_size, n * 2);
    memcpy((*darray)->values, new_nodes, node_size);
    (*darray)->values = new_nodes;
  }
  // insert
  (*darray)->values[(*darray)->count] = value;
  (*darray)->count++;
  (*darray)->nleft--;
  return 1;
}


/* 

  This is a merge_join algorithm used to
  allow two xts objects to be merged as one
  along a common index efficiently and fast

  The code is branched for REAL and INTEGER indexed values
  which allows for efficient memory usage and minimal
  testing/coercion

  
  Copyright Jeffrey A. Ryan 2008

*/
/* do_merge_xts {{{ */
SEXP do_merge_xts (SEXP x, SEXP y,
                   SEXP all,
                   SEXP fill,
                   SEXP retclass,
                   SEXP colnames, 
                   SEXP suffixes,
                   SEXP retside,
                   SEXP env,
                   int coerce)
{
  int nrx, ncx, nry, ncy;
  int left_join, right_join;
  int i = 0, j = 0, xp = 1, yp = 1; /* x and y positions in index */
  int mode;
  int ij_original, ij_result;
  int p = 0;
  SEXP xindex, yindex, index, result, attr, len_xindex;
  SEXP s, t, unique;

  /* we do not check that 'x' is an xts object.  Dispatch and mergeXts
    (should) make this unecessary.  So we just get the index value 

    This assumption seems to be invalid when dispatched from cbind.xts
    So we need to check that the objects are not NULL, or at least
    treat NULL objects as zero-width with an index that matches the non-null
   
    2009/01/07: calling merge(NA,x) or merge(1,1,xts) causes a segfault;
                calling merge(1,x) causes the xts-info (none!) from the 1st arg
                to be used, resulting in a classless object. [fixed - jar]
  */
  if( isNull(x) || isNull(y) ) {
    if(!isNull(x)) return(x);
    return(y);
  }

  PROTECT( xindex = getAttrib(x, xts_IndexSymbol) ); p++;

  /* convert to xts object if needed */
  if( !isXts(y) ) {
    PROTECT(s = t = allocList(4)); p++;
    SET_TYPEOF(s, LANGSXP);
    SETCAR(t, install("try.xts")); t = CDR(t);
    SETCAR(t, y); t = CDR(t);
    PROTECT( len_xindex = allocVector(INTSXP, 1)); p++;
    INTEGER(len_xindex)[0] = length(xindex);
    SETCAR(t, len_xindex);
    SET_TAG(t, install("length.out")); t = CDR(t);
    SETCAR(t, install(".merge.xts.scalar"));
    SET_TAG(t, install("error"));
    PROTECT(y = eval(s, env)); p++;
  } /* end conversion process */

  mode = TYPEOF(x);

  if( isXts(y) ) {
    PROTECT( yindex = getAttrib(y, xts_IndexSymbol) ); p++;
  } else {
    PROTECT( yindex = getAttrib(x, xts_IndexSymbol) ); p++;
  }

  if( TYPEOF(retside) != LGLSXP )
    error("retside must be a logical value of TRUE or FALSE");

  nrx = nrows(x);
  ncx = ncols(x);
  /* if object is zero-width */
  if( LENGTH(x)==0 || INTEGER(retside)[0]==0 ) {
    nrx = nrows(xindex);
    ncx = 0;
  }
  
  nry = nrows(y);
  ncy = ncols(y);
  /* if object is zero-width */
  if( LENGTH(y)==0 || INTEGER(retside)[1]==0) {
    nry = nrows(yindex);
    ncy = 0;
  }

  /* at present we are failing the call if the indexing is of
     mixed type.  This should probably instead simply coerce
     to REAL so as not to lose any information (at the expense
     of conversion cost and memory), and issue a warning. */
  if( TYPEOF(xindex) != TYPEOF(yindex) )
  {
    PROTECT(xindex = coerceVector(xindex, REALSXP)); p++;
    PROTECT(yindex = coerceVector(yindex, REALSXP)); p++;
  }

  if( TYPEOF(all) != LGLSXP )
    error("all must be a logical value of TRUE or FALSE");

  left_join = INTEGER(all)[ 0 ];
  right_join = INTEGER(all)[ 1 ];

  /* We support both double and integer indexes. Assign comparison
   * function based on which index type we are merging.
   */
  compare_func compare_indexes = NULL;
  xts_indices *idx = NULL;
//  if (getAttrib(xindex, R_ClassSymbol) == mkString("datetime")) {
//    compare_indexes = compare_indexes_long;
//    xts_index xd, yd;
//    xd.l = (int64_t *)REAL(xindex);
//    yd.l = (int64_t *)REAL(yindex);
//    idx = &(xts_indices){ xd, yd };
//    /* Check for illegal values before looping. Due to ordered index,
//     * -Inf must be first, while NA, Inf, and NaN must be last. */
//    //if (!R_FINITE(xd.d[0]) || !R_FINITE(xd.d[nrx-1])
//    // || !R_FINITE(yd.d[0]) || !R_FINITE(yd.d[nry-1])) {
//    //  error("'index' cannot contain 'NA', 'NaN', or '+/-Inf'");
//    //}
//
//  } else
  switch (TYPEOF(xindex)) {
    case REALSXP:
      compare_indexes = compare_indexes_double;
      xts_index xd, yd;
      xd.d = REAL(xindex);
      yd.d = REAL(yindex);
      idx = &(xts_indices){ xd, yd };
      /* Check for illegal values before looping. Due to ordered index,
       * -Inf must be first, while NA, Inf, and NaN must be last. */
      if (!R_FINITE(xd.d[0]) || !R_FINITE(xd.d[nrx-1])
       || !R_FINITE(yd.d[0]) || !R_FINITE(yd.d[nry-1])) {
        error("'index' cannot contain 'NA', 'NaN', or '+/-Inf'");
      }
      break;
    case INTSXP:
      compare_indexes = compare_indexes_int;
      xts_index xi, yi;
      xi.i = INTEGER(xindex);
      yi.i = INTEGER(yindex);
      idx = &(xts_indices){ xi, yi };
      /* Check for NA before looping; logical ops on NA may yield surprising
       * results. Note that the NA_integer_ will appear in the last value of
       * the index because of sorting at the R level, even though NA_INTEGER
       * equals INT_MIN at the C level. */
      if (xi.i[nrx-1] == NA_INTEGER || yi.i[nry-1] == NA_INTEGER) {
         error("'index' cannot contain 'NA'");
      }
      break;
    default:
      error("invalid index type");
      break;
  }

  /* determine num_rows of final merged xts object
     
     this seems to only cost 1/1000 of a sec per
     1e6 observations.  Acceptable 'waste' given
     that now we can properly allocate space
     for our results

     We also check the index type and use the appropriate macros
   */
  
  int nxnodes = 0;
  int nynodes = 0;

  // FIXME: use dynamic arrays: https://stackoverflow.com/a/46351960
  /*
   * struct int_list {
   *    size_t count;
   *    int value[];
   * };
   * int push_back(struct int_list **fubar, int value) {
   *    size_t x = *fubar ? fubar[0]->size : 0
   *         , y = x + 1;
   *
   *    if ((x & y) == 0) {
   *        void *temp = realloc(*fubar, sizeof **fubar
   *                                   + (x + y) * sizeof fubar[0]->value[0]);
   *        if (!temp) { return 1; }
   *        *fubar = temp; // or, if you like, `fubar[0] = temp;`
   *    }
   *
   *    fubar[0]->value[x] = value;
   *    fubar[0]->count = y;
   *    return 0;
   * }
   * struct int_list *array = NULL;
   */
  //xts_node xnodes[2048];
  //xts_node ynodes[2048];
  xts_node_darray *xnodes = NULL;
  xts_node_darray *ynodes = NULL;

  int yrun = 0;
  int xrun = 0;

  xts_node xnode = {0, 0, 0};
  xts_node ynode = {0, 0, 0};

  struct xts_node_darray *xnodesda;
  struct xts_node_darray *ynodesda;
  insert(&xnodesda, xnode);
  insert(&ynodesda, ynode);

  xp = 0; yp = 0;
  while (xp < nrx || yp < nry) {
//{{{ end of arrays
    if (xp >= nrx) {
      // determine if run for X needs to terminate (set result)
      if (xrun) {
        xrun = 0;
        xnode.num = xp - xnode.beg;
        xnodes[nxnodes++] = xnode;

insert(&xnodesda, xnode);
size_t ct = xnodesda->count;
xts_node nd = xnodesda->values[ct-1];
Rprintf("%d %d %d %d %d\n", ct, xnodesda->nleft, nd.beg+1, nd.out+1, nd.num);
        xnodes[nxnodes++] = xnode;
Rprintf("xp > nrx; xrun TRUE->FALSE; xnode %d %d %d\n", xnode.beg+1, xnode.out+1, xnode.num);

      }
      if (right_join) {
        // ensure first values are set for Y
        if (!yrun) {
          yrun = 1;
          ynode = (xts_node){yp, 0, i};
        }
        yp++;
        i++;
      } else {
        break;
      }
    } else

    if (yp >= nry) {
      // determine if run for Y needs to terminate (set result)
      if (yrun) {
        yrun = 0;
        ynode.num = yp - ynode.beg;
        ynodes[nynodes++] = ynode;
        //darray_insert(&ynodes, ynode);
      }
      if (left_join) {
        // ensure first values are set for X
        if (!xrun) {
          xrun = 1;
          xnode = (xts_node){xp, 0, i};
        }
        xp++;
        i++;
      } else {
        break;
      }
    } else {
//}}} end of arrays

      int comp = compare_indexes(idx, xp, yp);

//{{{ equal indexes
      if (comp == 0) {
        // ensure first values are set for X
        if (!xrun) {
          xrun = 1;
          xnode = (xts_node){xp, 0, i};
        }
        // ensure first values are set for Y
        if (!yrun) {
          yrun = 1;
          ynode = (xts_node){yp, 0, i};
        }

        // increment all values; none terminate
        yp++;
        xp++;
        i++;
      } else
  //}}} equal indexes
  //{{{ x < y
      if (comp < 0) {
        // determine if run for Y needs to terminate (set result)
        if (yrun) {
          yrun = 0;
          ynode.num = yp - ynode.beg;
          ynodes[nynodes++] = ynode;
          //darray_insert(&ynodes, ynode);
        }
        if (left_join) {
          // ensure first values are set for X
          if (!xrun) {
            xrun = 1;
            xnode = (xts_node){xp, 0, i};
          }
          i++;
        }
        xp++;
      } else
//}}} x < y
//{{{ x > y
      if (comp > 0) {
        // determine if run for X needs to terminate (set result)
        if (xrun) {
          xrun = 0;
          xnode.num = xp - xnode.beg;
          xnodes[nxnodes++] = xnode;
          //darray_insert(&xnodes, xnode);
        }
        if (right_join) {
          // ensure first values are set for Y
          if (!yrun) {
            yrun = 1;
            ynode = (xts_node){yp, 0, i};
          }
          i++;
        }
        yp++;
      } else
        error("Invalid index element comparison (should never happen)");
    }
  }
//}}} x > y
  // determine if run for X needs to terminate (set result)
  if (xrun) {
    xrun = 0;
    xnode.num = xp - xnode.beg;
    xnodes[nxnodes++] = xnode;
    //if (darray_insert(&xnodes, xnode)) {
    //  Rprintf("success\n");
    //} else {
    //  error("bad darray insert");
    //}
  }
  // determine if run for Y needs to terminate (set result)
  if (yrun) {
    yrun = 0;
    ynode.num = yp - ynode.beg;
    ynodes[nynodes++] = ynode;
    //if (darray_insert(&ynodes, ynode)) {
    //  Rprintf("success\n");
    //} else {
    //  error("bad darray insert");
    //}
  }

  if(i == 0) {
    /* if no rows match, return an empty xts object, similar in style to zoo */
    PROTECT( result = allocVector(TYPEOF(x), 0) ); p++;
    PROTECT( index  = allocVector(TYPEOF(xindex), 0) ); p++;
    SET_xtsIndex(result, index);
    if(LOGICAL(retclass)[0])
      setAttrib(result, R_ClassSymbol, getAttrib(x, R_ClassSymbol));
    UNPROTECT(p);
    return result;
  }

  int num_rows = i;
  xp = 1; yp = 1;

  PROTECT( index  = allocVector(TYPEOF(xindex), num_rows) ); p++;
  /* coercion/matching of TYPE for x and y needs to be checked,
     either here or in the calling R code.  I suspect here is
     more useful if other function can call the C code as well. 
     If objects are not the same type, convert to REALSXP. */
  if( coerce || TYPEOF(x) != TYPEOF(y) ) {
    PROTECT( x = coerceVector(x, REALSXP) ); p++;
    PROTECT( y = coerceVector(y, REALSXP) ); p++;
  }
  PROTECT( result = allocVector(TYPEOF(x), (ncx + ncy) * num_rows) ); p++;

  /* Copy index values from x and y to result */
  int m;
  xts_node mnode;
  switch (TYPEOF(index)) {
    case REALSXP:
      ;
      double *real_index = REAL(index);
      double *real_xindex = REAL(xindex);
      double *real_yindex = REAL(yindex);
      nxnodes = xnodes->count;
      nynodes = ynodes->count;
      for (m = 0; m < nxnodes; m++) {
        //mnode = xnodes[m];
        mnode = xnodes->values[m];
        memcpy(real_index + mnode.out,
            real_xindex + mnode.beg,
            mnode.num * sizeof(double));
      }
      for (m = 0; m < nynodes; m++) {
        //mnode = ynodes[m];
        mnode = ynodes->values[m];
        memcpy(real_index + mnode.out,
            real_yindex + mnode.beg,
            mnode.num * sizeof(double));
      }
      break;
    case INTSXP:
      ;
      int *int_index = INTEGER(index);
      int *int_xindex = INTEGER(xindex);
      int *int_yindex = INTEGER(yindex);
      nxnodes = xnodes->count;
      nynodes = ynodes->count;
      for (m = 0; m < nxnodes; m++) {
        //mnode = xnodes[m];
        mnode = xnodes->values[m];
        memcpy(int_index + mnode.out,
            int_xindex + mnode.beg,
            mnode.num * sizeof(int));
      }
      for (m = 0; m < nynodes; m++) {
        //mnode = ynodes[m];
        mnode = ynodes->values[m];
        memcpy(int_index + mnode.out,
            int_yindex + mnode.beg,
            mnode.num * sizeof(int));
      }
      break;
    default:
      error("unsupported index type");
      break;
  }

  /* Ensure fill is the correct length and type */
  if( length(fill) < 1 ) {
    PROTECT( fill = ScalarLogical(NA_LOGICAL) ); p++;
  }
  if( TYPEOF(fill) != TYPEOF(x) ) {
    PROTECT( fill = coerceVector(fill, TYPEOF(x)) ); p++;
  } 

  /* Copy data values from x and y to result */
  int result_obs = LENGTH(result);
  switch(TYPEOF(x)) {
    case INTSXP:
      ;
      int *int_x = INTEGER(x);
      int *int_y = INTEGER(y);
      int int_fill = INTEGER(fill)[0];
      int *int_result = INTEGER(result);
      for (i = 0; i < result_obs; i++) int_result[i] = int_fill;

      nxnodes = xnodes->count;
      nynodes = ynodes->count;
      for (m = 0; m < nxnodes; m++) {
        //mnode = xnodes[m];
        mnode = xnodes->values[m];
        for(j = 0; j < ncx; j++) { /* x-values */
          ij_result = j * num_rows;
          ij_original = j * nrx;
          memcpy(int_result + mnode.out + ij_result,
              int_x + mnode.beg + ij_original,
              mnode.num * sizeof(int));
        }
      }
      for (m = 0; m < nynodes; m++) {
        //mnode = ynodes[m];
        mnode = ynodes->values[m];
        for(j = 0; j < ncy; j++) { /* y-values */
          ij_result = (j+ncx) * num_rows;
          ij_original = j * nry;
          memcpy(int_result + mnode.out + ij_result,
              int_y + mnode.beg + ij_original,
              mnode.num * sizeof(int));
        }
      }
      break;
    case REALSXP:
      ;
      double *real_x = REAL(x);
      double *real_y = REAL(y);
      double real_fill = REAL(fill)[0];
      double *real_result = REAL(result);
      for (i = 0; i < result_obs; i++) real_result[i] = real_fill;

      nxnodes = xnodes->count;
      nynodes = ynodes->count;
      for (m = 0; m < nxnodes; m++) {
        //mnode = xnodes[m];
        mnode = xnodes->values[m];
        for(j = 0; j < ncx; j++) { /* x-values */
          ij_result = j * num_rows;
          ij_original = j * nrx;
          memcpy(real_result + mnode.out + ij_result,
              real_x + mnode.beg + ij_original,
              mnode.num * sizeof(double));
        }
      }
      for (m = 0; m < nynodes; m++) {
        //mnode = ynodes[m];
        mnode = ynodes->values[m];
        for(j = 0; j < ncy; j++) { /* y-values */
          ij_result = (j+ncx) * num_rows;
          ij_original = j * nry;
          memcpy(real_result + mnode.out + ij_result,
              real_y + mnode.beg + ij_original,
              mnode.num * sizeof(double));
        }
      }
      break;
    case LGLSXP:
      ;
      int *lgl_x = LOGICAL(x);
      int *lgl_y = LOGICAL(y);
      int lgl_fill = LOGICAL(fill)[0];
      int *lgl_result = LOGICAL(result);
      for (i = 0; i < result_obs; i++) lgl_result[i] = lgl_fill;

      nxnodes = xnodes->count;
      nynodes = ynodes->count;
      for (m = 0; m < nxnodes; m++) {
        //mnode = xnodes[m];
        mnode = xnodes->values[m];
        for(j = 0; j < ncx; j++) { /* x-values */
          ij_result = j * num_rows;
          ij_original = j * nrx;
          memcpy(lgl_result + mnode.out + ij_result,
              lgl_x + mnode.beg + ij_original,
              mnode.num * sizeof(int));
        }
      }
      for (m = 0; m < nynodes; m++) {
        //mnode = ynodes[m];
        mnode = ynodes->values[m];
        for(j = 0; j < ncy; j++) { /* y-values */
          ij_result = (j+ncx) * num_rows;
          ij_original = j * nry;
          memcpy(lgl_result + mnode.out + ij_result,
              lgl_y + mnode.beg + ij_original,
              mnode.num * sizeof(int));
        }
      }
      break;
    case CPLXSXP:
      ;
      Rcomplex *clpx_x = COMPLEX(x);
      Rcomplex *clpx_y = COMPLEX(y);
      Rcomplex clpx_fill = COMPLEX(fill)[0];
      Rcomplex *clpx_result = COMPLEX(result);
      for (i = 0; i < result_obs; i++) clpx_result[i] = clpx_fill;

      nxnodes = xnodes->count;
      nynodes = ynodes->count;
      for (m = 0; m < nxnodes; m++) {
        //mnode = xnodes[m];
        mnode = xnodes->values[m];
        for(j = 0; j < ncx; j++) { /* x-values */
          ij_result = j * num_rows;
          ij_original = j * nrx;
          memcpy(clpx_result + mnode.out + ij_result,
              clpx_x + mnode.beg + ij_original,
              mnode.num * 2 * sizeof(double));
        }
      }
      for (m = 0; m < nynodes; m++) {
        //mnode = ynodes[m];
        mnode = ynodes->values[m];
        for(j = 0; j < ncy; j++) { /* y-values */
          ij_result = (j+ncx) * num_rows;
          ij_original = j * nry;
          memcpy(clpx_result + mnode.out + ij_result,
              clpx_y + mnode.beg + ij_original,
              mnode.num * 2 * sizeof(double));
        }
      }
      break;
    case STRSXP:
      for (i = 0; i < result_obs; i++) {
        SET_STRING_ELT(result, i, STRING_ELT(fill, 0));
      }
      int n, result_n, original_n;
      nxnodes = xnodes->count;
      nynodes = ynodes->count;
      for (m = 0; m < nxnodes; m++) {
        //mnode = xnodes[m];
        mnode = xnodes->values[m];
        for (j = 0; j < ncx; j++) { /* x-values */
          ij_result = mnode.out + j * num_rows;
          ij_original = mnode.beg + j * nrx;
          for (n = 0; n < mnode.num; n++) {
            result_n = ij_result + n;
            original_n = ij_original + n;
            SET_STRING_ELT(result, result_n, STRING_ELT(x, original_n));
          }
        }
      }
      for (m = 0; m < nynodes; m++) {
        //mnode = ynodes[m];
        mnode = ynodes->values[m];
        for (j = 0; j < ncy; j++) { /* y-values */
          ij_result = mnode.out + (j+ncx) * num_rows;
          ij_original = mnode.beg + j * nry;
          for (n = 0; n < mnode.num; n++) {
            result_n = ij_result + n;
            original_n = ij_original + n;
            SET_STRING_ELT(result, result_n, STRING_ELT(y, original_n));
          }
        }
      }
      break;
    default:
      error("unsupported data type");
      break;
  }

  /* following logic to allow for 
     dimensionless xts objects (unsupported)
     to be used in Ops.xts calls
     This maps to how zoo behaves */
  if(LOGICAL(retside)[0] &&
     !LOGICAL(retside)[1] && 
     isNull(getAttrib(x,R_DimSymbol))) {
     /* retside=c(T,F) AND is.null(dim(x)) */ 
     setAttrib(result, R_DimSymbol, R_NilValue);
  } else 
  if(LOGICAL(retside)[1] &&
     !LOGICAL(retside)[0] && 
     isNull(getAttrib(y,R_DimSymbol))) {
     /* retside=c(F,T) AND is.null(dim(y)) */ 
     setAttrib(result, R_DimSymbol, R_NilValue);
  } else /* set Dim and DimNames */
  if(num_rows >= 0 && (ncx + ncy) >= 0) {
    /* DIM */
    PROTECT(attr = allocVector(INTSXP, 2));
    INTEGER(attr)[0] = num_rows;
    INTEGER(attr)[1] = ncx + ncy;
    setAttrib(result, R_DimSymbol, attr);
    UNPROTECT(1);
    /* DIMNAMES */
    if(!isNull(colnames)) { // only set DimNamesSymbol if passed colnames is not NULL
      SEXP dimnames, dimnames_x, dimnames_y, newcolnames;
      PROTECT(dimnames = allocVector(VECSXP, 2)); p++;
      PROTECT(dimnames_x = getAttrib(x, R_DimNamesSymbol)); p++;
      PROTECT(dimnames_y = getAttrib(y, R_DimNamesSymbol)); p++;
      PROTECT(newcolnames = allocVector(STRSXP, ncx+ncy)); p++;
      for(i = 0; i < (ncx + ncy); i++) {
        if( i < ncx ) {
          if(!isNull(dimnames_x) && !isNull(VECTOR_ELT(dimnames_x,1))) {
            SET_STRING_ELT(newcolnames, i, STRING_ELT(VECTOR_ELT(dimnames_x,1),i));
          } else {
            SET_STRING_ELT(newcolnames, i, STRING_ELT(colnames, i));
          }
        } else { // i >= ncx; 
          if(!isNull(dimnames_y) && !isNull(VECTOR_ELT(dimnames_y,1))) {
            SET_STRING_ELT(newcolnames, i, STRING_ELT(VECTOR_ELT(dimnames_y,1),i-ncx));
          } else {
            SET_STRING_ELT(newcolnames, i, STRING_ELT(colnames, i));
          }
        }
      }
      SET_VECTOR_ELT(dimnames, 0, R_NilValue);  // ROWNAMES are NULL

      PROTECT(s = t = allocList(3)); p++;
      SET_TYPEOF(s, LANGSXP);
      SETCAR(t, install("make.names")); t = CDR(t);
      SETCAR(t, newcolnames); t = CDR(t);
      PROTECT(unique = allocVector(LGLSXP, 1)); p++;  LOGICAL(unique)[0] = 1;
      SETCAR(t, unique);  SET_TAG(t, install("unique"));
      SET_VECTOR_ELT(dimnames, 1, eval(s, env));
 
      //SET_VECTOR_ELT(dimnames, 1, newcolnames); // COLNAMES are passed in
      setAttrib(result, R_DimNamesSymbol, dimnames);
    }
  } else {
    // only used for zero-width results! xts always has dimension
    setAttrib(result, R_DimSymbol, R_NilValue);
  }

  setAttrib(result, xts_IndexSymbol, index);
  if(LOGICAL(retclass)[0])
    setAttrib(result, R_ClassSymbol, getAttrib(x, R_ClassSymbol));
  setAttrib(result, xts_IndexClassSymbol, getAttrib(x, xts_IndexClassSymbol));
  setAttrib(result, xts_IndexTZSymbol, getAttrib(x, xts_IndexTZSymbol));
  setAttrib(result, xts_IndexFormatSymbol, getAttrib(x, xts_IndexFormatSymbol));
  setAttrib(result, xts_ClassSymbol, getAttrib(x, xts_ClassSymbol));
  copy_xtsAttributes(x, result);

  UNPROTECT(p);
  return result;  
} //}}}

//SEXP mergeXts (SEXP all, SEXP fill, SEXP retclass, SEXP colnames, SEXP retside, SEXP env, SEXP args)
/* called via .External("mergeXts", ...) */
SEXP mergeXts (SEXP args) // mergeXts {{{
{
  SEXP _x, _y, xtmp, result, _INDEX;
  /* colnames should be renamed as suffixes, as colnames need to be added at the C level */
  SEXP all, fill, retc, retclass, symnames,
       suffixes, rets, retside, env, tzone;
  int nr, nc, ncs=0;
  int index_len;
  int i, n=0, P=0;

  SEXP argstart;

  args = CDR(args); all = CAR(args);
  args = CDR(args); fill = CAR(args);
  args = CDR(args); retclass = CAR(args);
  args = CDR(args); symnames = CAR(args);
  args = CDR(args); suffixes = CAR(args);
  args = CDR(args); retside = CAR(args);
  args = CDR(args); env = CAR(args);
  args = CDR(args); tzone = CAR(args);
  args = CDR(args);
  // args should now correspond to the ... objects we are looking to merge 
  argstart = args; // use this to rewind list...

  n = 0;
  int type_of;
  int coerce_to_double=0;
  if(args != R_NilValue) type_of = TYPEOF(CAR(args));
  while(args != R_NilValue) {
    if( length(CAR(args)) > 0 )
      ncs += ncols(CAR(args));
    if(TYPEOF(CAR(args)) != type_of)
      coerce_to_double = 1;  /* need to convert all objects if one needs to be converted */
    args = CDR(args);
    n++;
  }


  /* build an index to be used in all subsequent calls */
  args = argstart;

  _x = CAR(args);
  args = CDR(args);

  int leading_non_xts = 0;
  while( !isXts(_x) ) {
    if( args == R_NilValue ) error("no xts object to merge");
    leading_non_xts = 1;
    /*warning("leading non-xts objects may have been dropped");*/
    _x = CAR(args);
    args = CDR(args);
  }
  /* test for NULLs that may be present from cbind dispatch */
  if(!leading_non_xts) { /* leading non-xts in 2 case scenario was igoring non-xts value */
    if(n < 3 && (args == R_NilValue || (isNull(CAR(args)) && length(args) == 1))) {/* no y arg or y==NULL */
      return(_x);
    }
  }

  if( args != R_NilValue) {
    _y = CAR(args);
    args = CDR(args);
  } else {
    PROTECT(_y = duplicate(_x)); P++;
  }

  if(n > 2 || leading_non_xts) { /*args != R_NilValue) {*/
    /* generalized n-case optimization
       currently if n>2 this is faster and more memory efficient
       than recursively building a merged object, object by object. */

    PROTECT(retc = allocVector(LGLSXP, 1)); P++;
    LOGICAL(retc)[0] = 1; /* return class == TRUE */
    PROTECT(rets = allocVector(LGLSXP, 2)); P++;
    LOGICAL(rets)[0] = 0; /* don't return left */
    LOGICAL(rets)[1] = 0; /* don't return right */
  
    if( isNull(_y) ) {
      PROTECT(_y = duplicate(_x)); P++;
    }

    // REPROTECT _INDEX in while loop
    PROTECT_INDEX idx;
    PROTECT_WITH_INDEX(_INDEX = do_merge_xts(_x,
                                             _y,
                                             all,
                                             fill,
                                             retc,
                                             R_NilValue,
                                             R_NilValue,
                                             rets,
                                             env,
                                             coerce_to_double), &idx); P++;

    /* merge all objects into one zero-width common index */
    while(args != R_NilValue) { 
      if( !isNull(CAR(args)) ) {
        REPROTECT(_INDEX = do_merge_xts(_INDEX,
                                        CAR(args),
                                        all,
                                        fill,
                                        retc,
                                        R_NilValue,
                                        R_NilValue,
                                        rets,
                                        env,
                                        coerce_to_double), idx);
      }
      args = CDR(args);
    }

    index_len = length(GET_xtsIndex(_INDEX));
  
    args = argstart; // reset args
    int ii, jj, iijj, jj_result;
    int *int_result=NULL, *int_xtmp=NULL;
    double *real_result=NULL, *real_xtmp=NULL;

    PROTECT(result = allocVector(TYPEOF(_INDEX), index_len * ncs)); P++;
    switch(TYPEOF(result)) {
      case LGLSXP:
      case INTSXP:
        int_result = INTEGER(result);
        break;
      case REALSXP:
        real_result = REAL(result);
        break;
      default:
        error("unsupported data type");
    }

    SEXP ColNames, NewColNames;
    PROTECT(NewColNames = allocVector(STRSXP, ncs)); P++;
    ncs = 0;
    // REPROTECT xtmp inside for loop
    PROTECT_INDEX idxtmp;
    PROTECT_WITH_INDEX(xtmp = NULL, &idxtmp); P++;

    for(i = 0, nc=0; args != R_NilValue; i = i+nc, args = CDR(args)) { // merge each object with index
      // i is object current being merged/copied
      // nc is offset in current object
      if( isNull(CAR(args)) ) {
        i = i-nc;
        continue;  // if NULL is passed, skip to the next object.
      }

      REPROTECT(xtmp = do_merge_xts(_INDEX,
                                    CAR(args),
                                    all,
                                    fill,
                                    retclass,
                        /*colnames*/R_NilValue,
                                    R_NilValue,
                                    retside,
                                    env,
                                    coerce_to_double), idxtmp);

      nr = nrows(xtmp);
      nc = (0 == nr) ? 0 : ncols(xtmp);  // ncols(numeric(0)) == 1
      ncs += nc;
      PROTECT(ColNames = getAttrib(CAR(args),R_DimNamesSymbol)); P++;
      switch(TYPEOF(xtmp)) { // by type, insert merged data into result object
        case LGLSXP:
        case INTSXP:
          int_xtmp = INTEGER(xtmp);
          for(jj=0; jj < nc; jj++) {
            if(!isNull(ColNames) && !isNull(VECTOR_ELT(ColNames,1))) {
              /* if merged object has colnames, use these, otherwise use deparse names */
              SET_STRING_ELT(NewColNames, i+jj, STRING_ELT(VECTOR_ELT(ColNames,1),jj));
            } else {
              SET_STRING_ELT(NewColNames, i+jj, STRING_ELT(symnames,i+jj));
            }
            for(ii=0; ii < nr; ii++) {
              iijj = ii + jj * nr;
              jj_result = ii + ( (i+jj) * nr);
              int_result[ jj_result ] = int_xtmp[ iijj ];
            }
          }
          break;
        case REALSXP:
          real_xtmp = REAL(xtmp);
          for(jj=0; jj < nc; jj++) {
            if(!isNull(ColNames) && !isNull(VECTOR_ELT(ColNames,1))) {
              SET_STRING_ELT(NewColNames, i+jj, STRING_ELT(VECTOR_ELT(ColNames,1),jj));
            } else {
              SET_STRING_ELT(NewColNames, i+jj, STRING_ELT(symnames,i+jj));
            }
            for(ii=0; ii < nr; ii++) {
              iijj = ii + jj * nr;
              jj_result = ii + ( (i+jj) * nr);
              real_result[ jj_result ] = real_xtmp[ iijj ];
            }
          }
          break;
      }
    }

    if(ncs > 0) {
      SEXP dim;
      PROTECT(dim = allocVector(INTSXP, 2)); P++;
      INTEGER(dim)[0] = index_len;
      INTEGER(dim)[1] = ncs;
      setAttrib(result, R_DimSymbol, dim);

      SEXP dimnames;
      PROTECT(dimnames = allocVector(VECSXP, 2)); P++;
      SET_VECTOR_ELT(dimnames, 0, R_NilValue); // rownames are always NULL in xts

      /* colnames, assure they are unique before returning */
      SEXP s, t, unique;
      PROTECT(s = t = allocList(3)); P++;
      SET_TYPEOF(s, LANGSXP);
      SETCAR(t, install("make.names")); t = CDR(t);
      SETCAR(t, NewColNames); t = CDR(t);
      PROTECT(unique = allocVector(LGLSXP, 1)); P++;  LOGICAL(unique)[0] = 1;
      SETCAR(t, unique);  SET_TAG(t, install("unique"));
      SET_VECTOR_ELT(dimnames, 1, eval(s, env));
      setAttrib(result, R_DimNamesSymbol, dimnames);
    }

    SET_xtsIndex(result, GET_xtsIndex(_INDEX));
    SET_xtsIndexTZ(result, GET_xtsIndexTZ(_INDEX));
    copy_xtsCoreAttributes(_INDEX, result);
    copy_xtsAttributes(_INDEX, result);

  } else { /* 2-case optimization --- simply call main routine */
    /* likely bug in handling of merge(1, xts) case */
    PROTECT(result = do_merge_xts(_x,
                                  _y, 
                                 all,
                                fill,
                            retclass,
                            symnames /*R_NilValue*/,
                            suffixes,
                             retside,
                                 env,
                    coerce_to_double)); P++;
  }

  SEXP index_tmp = getAttrib(result, xts_IndexSymbol);
  PROTECT(index_tmp); P++;
  if(isNull(tzone)) {
    setAttrib(index_tmp, xts_IndexTzoneSymbol,
              getAttrib(getAttrib(_x,xts_IndexSymbol), xts_IndexTzoneSymbol));
  } else {
    setAttrib(index_tmp, xts_IndexTzoneSymbol, tzone);
  }
  copyMostAttrib(getAttrib(_x,xts_IndexSymbol), index_tmp);
  setAttrib(result, xts_IndexSymbol, index_tmp);
  setAttrib(result, xts_IndexTZSymbol, getAttrib(index_tmp, xts_IndexTzoneSymbol));

  UNPROTECT(P);
  return(result);
} //}}} end of mergeXts
