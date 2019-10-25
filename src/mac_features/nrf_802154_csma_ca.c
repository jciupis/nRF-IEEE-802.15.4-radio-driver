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
 *   This file implements CSMA-CA procedure for the 802.15.4 driver.
 *
 */

#include "nrf_802154_csma_ca.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "nrf_802154_config.h"
#include "nrf_802154_const.h"
#include "nrf_802154_debug.h"
#include "nrf_802154_notification.h"
#include "nrf_802154_pib.h"
#include "nrf_802154_procedures_duration.h"
#include "nrf_802154_request.h"
#include "mac_features/nrf_802154_frame_parser.h"
#include "platform/random/nrf_802154_random.h"
#include "rsch/nrf_802154_rsch.h"
#include "timer_scheduler/nrf_802154_timer_sched.h"

#if NRF_802154_CSMA_CA_ENABLED

static uint8_t m_nb;                     ///< The number of times the CSMA-CA algorithm was required to back off while attempting the current transmission.
static uint8_t m_be;                     ///< Backoff exponent, which is related to how many backoff periods a device shall wait before attempting to assess a channel.

static const uint8_t * mp_data;          ///< Pointer to a buffer containing PHR and PSDU of the frame being transmitted.
static bool            m_is_running;     ///< Indicates if CSMA-CA procedure is running.
static uint32_t        m_frame_duration; ///< Duration of the frame being transmitted in microseconds.

/**
 * @brief Perform appropriate actions for busy channel conditions.
 *
 * According to CSMA-CA description in 802.15.4 specification, when channel is busy NB and BE shall
 * be incremented and the device shall wait random delay before next CCA procedure. If NB reaches
 * macMaxCsmaBackoffs procedure fails.
 *
 * @retval true   Procedure failed and TX failure should be notified to the next higher layer.
 * @retval false  Procedure is still ongoing and TX failure should be handled internally.
 */
static bool channel_busy(void);

/**
 * @brief Check if CSMA-CA is ongoing.
 *
 * @retval true   CSMA-CA is running.
 * @retval false  CSMA-CA is not running currently.
 */
static bool procedure_is_running(void)
{
    return m_is_running;
}

/**
 * @brief Stop CSMA-CA procedure.
 */
static void procedure_stop(void)
{
    nrf_802154_rsch_delayed_timeslot_cancel(RSCH_DLY_CSMACA);
    m_is_running = false;
}

/**
 * @brief Notify MAC layer that channel is busy if tx request failed and there are no retries left.
 *
 * @param[in]  result  Result of TX request.
 */
static void notify_busy_channel(bool result)
{
    if (!result && (m_nb >= (NRF_802154_CSMA_CA_MAX_CSMA_BACKOFFS - 1)))
    {
        nrf_802154_notify_transmit_failed(mp_data, NRF_802154_TX_ERROR_BUSY_CHANNEL);
    }
}

/**
 * @brief Perform CCA procedure followed by frame transmission.
 *
 * If transmission is requested, CSMA-CA module waits for notification from the FSM module.
 * If transmission request fails, CSMA-CA module performs procedure for busy channel condition
 * @sa channel_busy().
 *
 * @param[in] p_context  Unused variable passed from the Timer Scheduler module.
 */
static void frame_transmit(rsch_dly_ts_id_t dly_ts_id)
{
    (void)dly_ts_id;

    nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_CSMA_FRAME_TRANSMIT);

    if (procedure_is_running())
    {
        if (!nrf_802154_request_transmit(NRF_802154_TERM_NONE,
                                         REQ_ORIG_CSMA_CA,
                                         mp_data,
                                         true,
                                         NRF_802154_CSMA_CA_WAIT_FOR_TIMESLOT ? false : true,
                                         notify_busy_channel))
        {
            (void)channel_busy();
        }
    }

    nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_CSMA_FRAME_TRANSMIT);
}

/**
 * @brief Delay CCA procedure for random (2^BE - 1) unit backoff periods.
 *
 * @param[in] allow_zero_backoff  Flag that indicates if backoff equal to 0 can be allowed for.
 */
static void random_backoff_start(bool allow_zero_backoff)
{
    uint8_t backoff_periods;

    do
    {
        backoff_periods = nrf_802154_random_get() % (1 << m_be);
    }
    while (!allow_zero_backoff && (0 == backoff_periods));

    if (0 == backoff_periods)
    {
        // Don't schedule a delayed operation, because its target time would be in the past anyway.
        // Attempt to transmit straight away instead
        frame_transmit(RSCH_DLY_CSMACA);
    }
    else
    {
        rsch_dly_ts_param_t backoff_ts_param =
        {
            .t0                = nrf_802154_timer_sched_time_get(),
            .dt                = backoff_periods * UNIT_BACKOFF_PERIOD,
            .length            = m_frame_duration,
            .prio              = RSCH_PRIO_IDLE_LISTENING,
            .id                = RSCH_DLY_CSMACA,
            .prec_req_strategy = RSCH_PREC_REQ_STRATEGY_MANUAL,
            .started_callback  = &frame_transmit,
        };

        // If Coex precondition should be requested immediately, preconditions priority must be leveraged
        if (nrf_802154_pib_coex_tx_request_mode_get() == NRF_802154_COEX_TX_REQUEST_FRAME_READY)
        {
            backoff_ts_param.prio = RSCH_PRIO_TX;
        }

        // If backoff was not scheduled successfully, handle it like a failed attempt
        if (!nrf_802154_rsch_delayed_timeslot_request(&backoff_ts_param))
        {
            channel_busy();
        }
    }
}

/**
 * @brief Reschedule a transmission attempt with random backoff time.
 *
 * param[in] allow_zero_backoff  Flag that indicates if backoff equal to 0 can be allowed for.
 */
static bool reschedule(bool allow_zero_backoff)
{
    m_nb++;

    if (m_be < NRF_802154_CSMA_CA_MAX_BE)
    {
        m_be++;
    }

    if (m_nb < NRF_802154_CSMA_CA_MAX_CSMA_BACKOFFS)
    {
        random_backoff_start(allow_zero_backoff);
        return false;
    }
    else
    {
        procedure_stop();
        return true;
    }
}

static bool channel_busy(void)
{
    bool result = true;

    if (procedure_is_running())
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_CSMA_CHANNEL_BUSY);

        result = reschedule(true);

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_CSMA_CHANNEL_BUSY);
    }

    return result;
}

/**
 * @brief Perform appropriate actions when a transmission attempt fails because it's aborted
 *        by another operation.
 */
static bool backoff_aborted(void)
{
    bool result = true;

    if (procedure_is_running())
    {
        result = reschedule(false);
    }

    return result;
}

void nrf_802154_csma_ca_start(const uint8_t * p_data)
{
    assert(!procedure_is_running());

    mp_data          = p_data;
    m_nb             = 0;
    m_be             = NRF_802154_CSMA_CA_MIN_BE;
    m_is_running     = true;
    m_frame_duration = nrf_802154_tx_duration_get(p_data[0],
                                                  true,
                                                  nrf_802154_frame_parser_ar_bit_is_set(p_data));

    random_backoff_start(true);
}

bool nrf_802154_csma_ca_abort(nrf_802154_term_t term_lvl, req_originator_t req_orig)
{
    // Stop CSMA-CA only if request by the core or the higher layer.
    if ((req_orig != REQ_ORIG_CORE) && (req_orig != REQ_ORIG_HIGHER_LAYER))
    {
        return true;
    }

    bool result = true;

    nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_CSMA_ABORT);

    if (term_lvl >= NRF_802154_TERM_802154)
    {
        // Stop CSMA-CA if termination level is high enough.
        procedure_stop();
    }
    else
    {
        // Return success in case procedure is already stopped.
        result = !procedure_is_running();
    }

    nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_CSMA_ABORT);

    return result;
}

bool nrf_802154_csma_ca_tx_failed_hook(const uint8_t * p_frame, nrf_802154_tx_error_t error)
{
    (void)error;

    bool result = true;

    if (p_frame == mp_data)
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_CSMA_TX_FAILED);

        if (error == NRF_802154_TX_ERROR_ABORTED)
        {
            result = backoff_aborted();
        }
        else
        {
            result = channel_busy();
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_CSMA_TX_FAILED);
    }

    return result;
}

bool nrf_802154_csma_ca_tx_started_hook(const uint8_t * p_frame)
{
    if (p_frame == mp_data)
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_CSMA_TX_STARTED);

        procedure_stop();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_CSMA_TX_STARTED);
    }

    return true;
}

#endif // NRF_802154_CSMA_CA_ENABLED
