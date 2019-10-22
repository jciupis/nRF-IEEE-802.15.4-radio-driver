/* Copyright (c) 2017 - 2019, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this
 *      list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice,
 *      this list of conditions and the following disclaimer in the documentation
 *      and/or other materials provided with the distribution.
 *
 *   3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * @file
 *   This file implements SWI manager for nRF 802.15.4 driver.
 *
 */

#include "nrf_802154_swi.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "compiler_abstraction.h"
#include "nrf_802154_config.h"
#include "nrf_802154_peripherals.h"
#include "nrf_802154_utils.h"

#define SWI_EGU        NRF_802154_SWI_EGU_INSTANCE ///< Label of SWI peripheral.
#define SWI_IRQn       NRF_802154_SWI_IRQN         ///< Symbol of SWI IRQ number.
#define SWI_IRQHandler NRF_802154_SWI_IRQ_HANDLER  ///< Symbol of SWI IRQ handler.

void nrf_802154_swi_init(void)
{
#if !NRF_IS_IRQ_PRIORITY_ALLOWED(NRF_802154_SWI_PRIORITY)
#error NRF_802154_SWI_PRIORITY value out of the allowed range.
#endif
    NVIC_SetPriority(SWI_IRQn, NRF_802154_SWI_PRIORITY);
    NVIC_ClearPendingIRQ(SWI_IRQn);
    NVIC_EnableIRQ(SWI_IRQn);
}

__WEAK void nrf_802154_trx_swi_irq_handler(void)
{
    /* Implementation provided by other module if necessary */
}

__WEAK void nrf_802154_notification_swi_irq_handler(void)
{
    /* Implementation provided by other module if necessary */
}

__WEAK void nrf_802154_priority_drop_swi_irq_handler(void)
{
    /* Implementation provided by other module if necessary */
}

__WEAK void nrf_802154_request_swi_irq_handler(void)
{
    /* Implementation provided by other module if necessary */
}

void SWI_IRQHandler(void)
{
    nrf_802154_trx_swi_irq_handler();
    nrf_802154_notification_swi_irq_handler();
    nrf_802154_priority_drop_swi_irq_handler();
    nrf_802154_request_swi_irq_handler();
}
