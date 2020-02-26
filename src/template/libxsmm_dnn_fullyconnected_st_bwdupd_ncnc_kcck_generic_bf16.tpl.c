/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Evangelos Georganas, Alexander Heinecke (Intel Corp.)
******************************************************************************/
if (handle->bwdupd_divergent_par == 0) {
#include "libxsmm_dnn_fullyconnected_st_bwdupd_ncnc_kcck_generic_bf16_no_divergent_par.tpl.c"
} else {
#include "libxsmm_dnn_fullyconnected_st_bwdupd_ncnc_kcck_generic_bf16_divergent_par.tpl.c"
}
