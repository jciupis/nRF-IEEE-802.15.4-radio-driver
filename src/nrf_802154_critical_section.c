/* Copyright (c) 2017 - 2018, Nordic Semiconductor ASA
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
 *   This file implements critical sections used with requests by 802.15.4 driver.
 *
 */

#include "nrf_802154_critical_section.h"

#include <assert.h>
#include <stdint.h>

#include "nrf_802154_config.h"
#include "nrf_802154_debug.h"
#include "nrf_radio.h"
#include "rsch/nrf_802154_rsch.h"
#include "platform/lp_timer/nrf_802154_lp_timer.h"

#include <nrf.h>

#define CMSIS_IRQ_NUM_VECTACTIVE_DIFF 16                           ///< Offset in exception number of external interrupts according to ARM Archuitecture Reference Manual

static volatile uint8_t m_nested_critical_section_counter;         ///< Counter of nested critical sections
static volatile uint8_t m_nested_critical_section_current_context; ///< Execution context of the current critical section

/***************************************************************************************************
 * @section Critical sections management
 **************************************************************************************************/

/** @brief Enter critical section for RADIO peripheral
 *
 * @note RADIO peripheral registers (and NVIC) are modified only when timeslot is granted for the
 *       802.15.4 driver.
 */
static void radio_critical_section_enter(void)
{
    if (nrf_802154_rsch_prec_is_approved(RSCH_PREC_RAAL, RSCH_PRIO_MIN_APPROVED))
    {
        NVIC_DisableIRQ(RADIO_IRQn);
    }
}

/** @brief Exit critical section for RADIO peripheral
 *
 * @note RADIO peripheral registers (and NVIC) are modified only when timeslot is granted for the
 *       802.15.4 driver.
 */
static void radio_critical_section_exit(void)
{
    if (nrf_802154_rsch_prec_is_approved(RSCH_PREC_RAAL, RSCH_PRIO_MIN_APPROVED))
    {
        NVIC_EnableIRQ(RADIO_IRQn);
    }
}

/** @brief Get the context the code is currently executed from.
 *
 * @return External interrupt number with @ref CMSIS_IRQ_NUM_VECTACTIVE_DIFF offset in case of
 *         an interrupt context or 0 when the code is executed from the main thread.
 */
static uint8_t current_execution_context_get(void)
{
    return (uint8_t)((SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) >> SCB_ICSR_VECTACTIVE_Pos);
}

static bool critical_section_enter(void)
{
    uint8_t  counter;
    bool     result          = false;
    uint32_t interrupt_state = __get_PRIMASK();

    __disable_irq();
    __DSB();
    __ISB();

    // Operate on local copies of the volatile variables for faster execution.
    counter = m_nested_critical_section_counter;

    // Enter critical section only if it is free or occupied by the same context.
    if (counter == 0)
    {
        counter++;

        nrf_802154_critical_section_rsch_enter();
        nrf_802154_lp_timer_critical_section_enter();
        radio_critical_section_enter();

        m_nested_critical_section_counter         = counter;
        m_nested_critical_section_current_context = current_execution_context_get();

        result = true;
    }
    else if (m_nested_critical_section_current_context == current_execution_context_get())
    {
        counter++;
        m_nested_critical_section_counter = counter;

        result = true;
    }
    else
    {
        // Intentionally empty
    }

    __set_PRIMASK(interrupt_state);

    return result;
}

static void critical_section_exit(void)
{
    bool     retry = false;
    uint8_t  counter;
    uint32_t interrupt_state = __get_PRIMASK();

    // Assert that the caller has the right to exit the critical section.
    assert(m_nested_critical_section_current_context == current_execution_context_get());

    // Operate on local copy of the volatile variable for faster execution.
    counter = m_nested_critical_section_counter;

    // Assert that every exit() call is paired with a successful enter() call
    assert(counter > 0);

    do
    {
        // Exit RSCH critical section with enabled interrupts (it might take some time).
        if (counter == 1)
        {
            nrf_802154_critical_section_rsch_exit();
        }

        // Exit radio & LP timer critical sections atomically.
        __disable_irq();
        __DSB();
        __ISB();

        if (counter == 1)
        {
            radio_critical_section_exit();
            nrf_802154_lp_timer_critical_section_exit();
        }

        counter--;
        m_nested_critical_section_counter = counter;

        __set_PRIMASK(interrupt_state);

        // It might have happened that after exiting RSCH critical section, higher priority
        // interrupt caused a change of state. Such case is handled below
        if (nrf_802154_critical_section_rsch_event_is_pending())
        {
            bool result = critical_section_enter();

            assert(result);
            (void)result;

            // Since critical section was entered, counter should be incremented to reflect the
            // current value of the static counter. It's not an assignment to speed up the execution
            counter++;

            retry = true;
        }
        else
        {
            retry = false;
        }
    }
    while (retry);
}

/***************************************************************************************************
 * @section API functions
 **************************************************************************************************/

void nrf_802154_critical_section_init(void)
{
    m_nested_critical_section_counter         = 0;
    m_nested_critical_section_current_context = 0;
}

bool nrf_802154_critical_section_enter(void)
{
    bool result;

    nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_CRIT_SECT_ENTER);

    result = critical_section_enter();

    nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_CRIT_SECT_ENTER);

    return result;
}

void nrf_802154_critical_section_exit(void)
{
    nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_CRIT_SECT_EXIT);

    critical_section_exit();

    nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_CRIT_SECT_EXIT);
}

bool nrf_802154_critical_section_is_nested(void)
{
    return m_nested_critical_section_counter > 1;
}

uint32_t nrf_802154_critical_section_active_vector_priority_get(void)
{
    uint8_t active_vector_id = current_execution_context_get();

    if (active_vector_id == 0)
    {
        // This function is called from the main thread.
        return UINT32_MAX;
    }
    else
    {
        // This function is called from an interrupt with the number below
        assert(active_vector_id >= CMSIS_IRQ_NUM_VECTACTIVE_DIFF);
        return NVIC_GetPriority((IRQn_Type)(active_vector_id - CMSIS_IRQ_NUM_VECTACTIVE_DIFF));
    }
}
