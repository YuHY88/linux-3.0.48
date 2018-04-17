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
 @File          dtsec_mii_acc.c

 @Description   FM dtsec MII register access MAC ...
*//***************************************************************************/

#include "error_ext.h"
#include "std_ext.h"
#include "fm_mac.h"
#include "dtsec.h"


static uint8_t GetMiiDiv(int32_t refClk)
{
    uint32_t    div,tmpClk;
    int         minRange;

    div = 1;
    minRange = (int)(refClk/40 - 1);

    tmpClk = (uint32_t)ABS(refClk/60 - 1);
    if (tmpClk < minRange)
    {
        div = 2;
        minRange = (int)tmpClk;
    }
    tmpClk = (uint32_t)ABS(refClk/60 - 1);
    if (tmpClk < minRange)
    {
        div = 3;
        minRange = (int)tmpClk;
    }
    tmpClk = (uint32_t)ABS(refClk/80 - 1);
    if (tmpClk < minRange)
    {
        div = 4;
        minRange = (int)tmpClk;
    }
    tmpClk = (uint32_t)ABS(refClk/100 - 1);
    if (tmpClk < minRange)
    {
        div = 5;
        minRange = (int)tmpClk;
    }
    tmpClk = (uint32_t)ABS(refClk/140 - 1);
    if (tmpClk < minRange)
    {
        div = 6;
        minRange = (int)tmpClk;
    }
    tmpClk = (uint32_t)ABS(refClk/280 - 1);
    if (tmpClk < minRange)
    {
        div = 7;
        minRange = (int)tmpClk;
    }

    return (uint8_t)div;
}


/*****************************************************************************/
t_Error DTSEC_MII_WritePhyReg(t_Handle    h_Dtsec,
                              uint8_t     phyAddr,
                              uint8_t     reg,
                              uint16_t    data)
{
    t_Dtsec             *p_Dtsec = (t_Dtsec *)h_Dtsec;
    t_MiiAccessMemMap   *p_MiiAccess;
    uint32_t            tmpReg;

    SANITY_CHECK_RETURN_ERROR(p_Dtsec, E_INVALID_HANDLE);
    SANITY_CHECK_RETURN_ERROR(p_Dtsec->p_MiiMemMap, E_INVALID_HANDLE);

    p_MiiAccess = p_Dtsec->p_MiiMemMap;

    WRITE_UINT32(p_Dtsec->p_MiiMemMap->miimcfg,
                 (uint32_t)GetMiiDiv((int32_t)(((p_Dtsec->fmMacControllerDriver.clkFreq*10)/2)/8)));

    CORE_MemoryBarrier();

    /* Stop the MII management read cycle */
    WRITE_UINT32(p_MiiAccess->miimcom, 0);
    /* Dummy read to make sure MIIMCOM is written */
    tmpReg = GET_UINT32(p_MiiAccess->miimcom);

    /* Setting up MII Management Address Register */
    tmpReg = (uint32_t)((phyAddr << MIIMADD_PHY_ADDR_SHIFT) | reg);
    WRITE_UINT32(p_MiiAccess->miimadd, tmpReg);

    /* Setting up MII Management Control Register with data */
    WRITE_UINT32(p_MiiAccess->miimcon, (uint32_t)data);
    /* Dummy read to make sure MIIMCON is written */
    tmpReg = GET_UINT32(p_MiiAccess->miimcon);

    CORE_MemoryBarrier();

    /* Wait till MII management write is complete */
    while ((GET_UINT32(p_MiiAccess->miimind)) & MIIMIND_BUSY) ;

    return E_OK;
}

/*****************************************************************************/
t_Error DTSEC_MII_ReadPhyReg(t_Handle h_Dtsec,
                             uint8_t  phyAddr,
                             uint8_t  reg,
                             uint16_t *p_Data)
{
    t_Dtsec             *p_Dtsec = (t_Dtsec *)h_Dtsec;
    t_MiiAccessMemMap   *p_MiiAccess;
    uint32_t            tmpReg;

    SANITY_CHECK_RETURN_ERROR(p_Dtsec, E_INVALID_HANDLE);
    SANITY_CHECK_RETURN_ERROR(p_Dtsec->p_MiiMemMap, E_INVALID_HANDLE);

    p_MiiAccess = p_Dtsec->p_MiiMemMap;

    WRITE_UINT32(p_Dtsec->p_MiiMemMap->miimcfg,
                 (uint32_t)GetMiiDiv((int32_t)(((p_Dtsec->fmMacControllerDriver.clkFreq*10)/2)/8)));

    CORE_MemoryBarrier();

    /* Setting up the MII Management Address Register */
    tmpReg = (uint32_t)((phyAddr << MIIMADD_PHY_ADDR_SHIFT) | reg);
    WRITE_UINT32(p_MiiAccess->miimadd, tmpReg);

    /* Perform an MII management read cycle */
    WRITE_UINT32(p_MiiAccess->miimcom, MIIMCOM_READ_CYCLE);
    /* Dummy read to make sure MIIMCOM is written */
    tmpReg = GET_UINT32(p_MiiAccess->miimcom);

    CORE_MemoryBarrier();

    /* Wait till MII management read is complete */
    while ((GET_UINT32(p_MiiAccess->miimind)) & MIIMIND_BUSY) ;

    /* Read MII management status  */
    *p_Data = (uint16_t)GET_UINT32(p_MiiAccess->miimstat);

    WRITE_UINT32(p_MiiAccess->miimcom, 0);
    /* Dummy read to make sure MIIMCOM is written */
    tmpReg = GET_UINT32(p_MiiAccess->miimcom);

    if (*p_Data == 0xffff)
        RETURN_ERROR(MINOR, E_NO_DEVICE,
                     ("Read wrong data (0xffff): phyAddr 0x%x, reg 0x%x",
                      phyAddr, reg));

    return E_OK;
}
