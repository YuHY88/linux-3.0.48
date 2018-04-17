/*
 * Copyright 2008-2012 Freescale Semiconductor Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Freescale Semiconductor nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License or (at your option) any
 * later version.
 *
 * THIS SOFTWARE IS PROVIDED BY Freescale Semiconductor ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Freescale Semiconductor BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


/******************************************************************************
 @File          fm_sp_common.h

 @Description   FM SP  ...
*//***************************************************************************/
#ifndef __FM_SP_COMMON_H
#define __FM_SP_COMMON_H

#include "std_ext.h"
#include "error_ext.h"
#include "list_ext.h"

#include "fm_ext.h"
#include "fm_pcd_ext.h"


/* sizes */
#define CAPWAP_FRAG_EXTRA_SPACE                 32
#define OFFSET_UNITS                            16
#define MAX_INT_OFFSET                          240
#define MAX_IC_SIZE                             256
#define MAX_EXT_OFFSET                          496
#define MAX_EXT_BUFFER_OFFSET                   511

/**************************************************************************//**
 @Description       defaults
*//***************************************************************************/
#define DEFAULT_FM_SP_bufferPrefixContent_privDataSize      0
#define DEFAULT_FM_SP_bufferPrefixContent_passPrsResult     FALSE
#define DEFAULT_FM_SP_bufferPrefixContent_passTimeStamp     FALSE
#define DEFAULT_FM_SP_bufferPrefixContent_allOtherPCDInfo   FALSE
#define DEFAULT_FM_SP_bufferPrefixContent_dataAlign         64
#define DEFAULT_FM_SP_dmaSwapData                           e_FM_DMA_NO_SWP
#define DEFAULT_FM_SP_dmaIntContextCacheAttr                e_FM_DMA_NO_STASH
#define DEFAULT_FM_SP_dmaHeaderCacheAttr                    e_FM_DMA_NO_STASH
#define DEFAULT_FM_SP_dmaScatterGatherCacheAttr             e_FM_DMA_NO_STASH
#define DEFAULT_FM_SP_dmaWriteOptimize                      TRUE
#define DEFAULT_FM_SP_noScatherGather                       FALSE

/**************************************************************************//**
 @Description       Registers bit fields
*//***************************************************************************/
#define FM_SP_EXT_BUF_POOL_EN_COUNTER             0x40000000
#define FM_SP_EXT_BUF_POOL_VALID                  0x80000000
#define FM_SP_EXT_BUF_POOL_BACKUP                 0x20000000
#define FM_SP_DMA_ATTR_WRITE_OPTIMIZE             0x00100000
#define FM_SP_SG_DISABLE                          0x80000000

/* shifts */
#define FM_SP_EXT_BUF_POOL_ID_SHIFT               16
#define FM_SP_POOL_DEP_NUM_OF_POOLS_SHIFT         16
#define FM_SP_EXT_BUF_MARG_START_SHIFT            16
#define FM_SP_EXT_BUF_MARG_END_SHIFT              0
#define FM_SP_DMA_ATTR_SWP_SHIFT                  30
#define FM_SP_DMA_ATTR_IC_CACHE_SHIFT             28
#define FM_SP_DMA_ATTR_HDR_CACHE_SHIFT            26
#define FM_SP_DMA_ATTR_SG_CACHE_SHIFT             24
#define FM_SP_IC_TO_EXT_SHIFT                     16
#define FM_SP_IC_FROM_INT_SHIFT                   8
#define FM_SP_IC_SIZE_SHIFT                       0


/**************************************************************************//**
 @Description   structure for defining internal context copying
*//***************************************************************************/
typedef struct
{
    uint16_t    extBufOffset;       /**< Offset in External buffer to which internal
                                         context is copied to (Rx) or taken from (Tx, Op). */
    uint8_t     intContextOffset;   /**< Offset within internal context to copy from
                                         (Rx) or to copy to (Tx, Op). */
    uint16_t    size;               /**< Internal offset size to be copied */
} t_FmSpIntContextDataCopy;

/**************************************************************************//**
 @Description   struct for defining external buffer margins
*//***************************************************************************/
typedef struct {
    uint16_t    startMargins;           /**< Number of bytes to be left at the beginning
                                             of the external buffer (must be divisible by 16) */
    uint16_t    endMargins;             /**< number of bytes to be left at the end
                                             of the external buffer(must be divisible by 16) */
} t_FmSpBufMargins;

typedef struct {
    uint32_t      dataOffset;
    uint32_t      prsResultOffset;
    uint32_t      timeStampOffset;
    uint32_t      hashResultOffset;
    uint32_t      pcdInfoOffset;
    uint32_t      manipOffset;
} t_FmSpBufferOffsets;


t_Error        FmSpBuildBufferStructure(t_FmSpIntContextDataCopy      *p_FmPortIntContextDataCopy,
                                        t_FmBufferPrefixContent       *p_BufferPrefixContent,
                                        t_FmSpBufMargins              *p_FmPortBufMargins,
                                        t_FmSpBufferOffsets           *p_FmPortBufferOffsets,
                                        uint8_t                       *internalBufferOffset);

t_Error     FmSpCheckIntContextParams(t_FmSpIntContextDataCopy *p_FmSpIntContextDataCopy);
t_Error     FmSpCheckBufPoolsParams(t_FmExtPools *p_FmExtPools,
                                    t_FmBackupBmPools *p_FmBackupBmPools,
                                    t_FmBufPoolDepletion *p_FmBufPoolDepletion);
t_Error     FmSpCheckBufMargins(t_FmSpBufMargins *p_FmSpBufMargins);
void        FmSpSetBufPoolsInAscOrderOfBufSizes(t_FmExtPools *p_FmExtPools, uint8_t *orderedArray, uint16_t *sizesArray);

t_Error     FmPcdSpAllocProfiles(t_Handle h_FmPcd,
                                 uint8_t  hardwarePortId,
                                 uint16_t numOfStorageProfiles,
                                 uint16_t *base,
                                 uint8_t  *log2Num);
t_Error     FmPcdSpGetAbsoluteProfileId(t_Handle                        h_FmPcd,
                                        t_Handle                        h_FmPort,
                                        uint16_t                        relativeProfile,
                                        uint16_t                        *p_AbsoluteId);
void SpInvalidateProfileSw(t_Handle h_FmPcd, uint16_t absoluteProfileId);
void SpValidateProfileSw(t_Handle h_FmPcd, uint16_t absoluteProfileId);


#endif /* __FM_SP_COMMON_H */
