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
 *   This file implements the nrf 802.15.4 radio arbiter for softdevice.
 *
 * This arbiter should be used when 802.15.4 works concurrently with SoftDevice's radio stack.
 *
 */

#define NRF_802154_MODULE_ID NRF_802154_MODULE_ID_RAAL

#include "nrf_raal_softdevice.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <nrf_802154.h>
#include <nrf_802154_const.h>
#include "nrf_802154_debug.h"
#include <nrf_802154_procedures_duration.h>
#include <nrf_802154_utils.h>
#include <nrf_timer.h>
#include <rsch/raal/nrf_raal_api.h>

#if defined(__GNUC__)
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored \"-Wreturn-type\"")
_Pragma("GCC diagnostic ignored \"-Wunused-parameter\"")
_Pragma("GCC diagnostic ignored \"-Wpedantic\"")
#endif

#include <ble.h>
#include <nrf_mbr.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>

#if defined(__GNUC__)
_Pragma("GCC diagnostic pop")
#endif

/***************************************************************************************************
 * @section Defines and typedefs.
 **************************************************************************************************/

/*
 * @brief Defines the only version of the SoftDevice that supports configuration of BLE advertising
 *        role scheduling.
 *
 *        The only SoftDevice that supports this option is S140 6.1.1 (6001001). The full version
 *        number for the SoftDevice binary is a decimal number in the form Mmmmbbb, where:
 *           - M is major version (one or more digits)
 *           - mmm is minor version (three digits)
 *           - bbb is bugfix version (three digits).
 */
#define BLE_ADV_SCHED_CFG_SUPPORT_SD_VERSION         (6001001)

/*
 * @brief Defines the minimum version of the SoftDevice that correctly handles timeslot releasing.
 *
 *        The first SoftDevice that supports this option is S140 6.1.0 (6001000). The full version
 *        number for the SoftDevice binary is a decimal number in the form Mmmmbbb, where:
 *           - M is major version (one or more digits)
 *           - mmm is minor version (three digits)
 *           - bbb is bugfix version (three digits).
 */
#define TIMESLOT_RELEASE_SUPPORT_MIN_SD_VERSION      (6001000)

/**@brief Enable Request and End on timeslot safety interrupt. */
#define ENABLE_REQUEST_AND_END_ON_TIMESLOT_END       0

/**@brief RAAL Timer instance. */
#define RAAL_TIMER                                   NRF_TIMER0

/**@brief RAAL Timer interrupt number. */
#define RAAL_TIMER_IRQn                              TIMER0_IRQn

/**@brief Minimum time prior safe margin reached by RTC when TIMER reports reached margin in microseconds. */
#define MIN_TIME_PRIOR_MARGIN_IS_REACHED_US          31

/**@brief Timer compare channel definitions. */
#define TIMER_CC_ACTION                              NRF_TIMER_CC_CHANNEL0
#define TIMER_CC_ACTION_EVENT                        NRF_TIMER_EVENT_COMPARE0
#define TIMER_CC_ACTION_INT                          NRF_TIMER_INT_COMPARE0_MASK

#define TIMER_CC_CAPTURE                             NRF_TIMER_CC_CHANNEL1
#define TIMER_CC_CAPTURE_TASK                        NRF_TIMER_TASK_CAPTURE1

#define MINIMUM_TIMESLOT_LENGTH_EXTENSION_TIME_TICKS NRF_802154_US_TO_RTC_TICKS( \
        NRF_RADIO_MINIMUM_TIMESLOT_LENGTH_EXTENSION_TIME_US)

/**@brief PPM constants. */
#define PPM_UNIT                                     1000000UL
#define MAX_HFCLK_PPM                                40

/**@brief Defines states of timeslot. */
typedef enum
{
    TIMESLOT_STATE_IDLE = 0,
    TIMESLOT_STATE_DROPPED,
    TIMESLOT_STATE_REQUESTED,
    TIMESLOT_STATE_GRANTED
} timeslot_state_t;

/**@brief Define timer actions. */
typedef enum
{
    TIMER_ACTION_EXTEND,
    TIMER_ACTION_MARGIN,
} timer_action_t;

/**@brief Configuration of the RAAL. */
typedef struct
{
    nrf_raal_softdevice_cfg_t sd_cfg;                       ///< SoftDevice-related configuration.
    uint32_t                  extension_interval;           ///< Configured interval between successive timeslot extensions.
    bool                      timeslot_releasing_supported; ///< Flag that indicates if the provided SoftDevice version supports timeslot releasing.
} nrf_raal_cfg_t;

/***************************************************************************************************
 * @section Static variables.
 **************************************************************************************************/

/**@brief Defines if module has been initialized. */
static bool m_initialized = false;

/**@brief Return parameter for SD radio signal handler. */
static nrf_radio_signal_callback_return_param_t m_ret_param;

/**@brief Current configuration of the RAAL. */
static nrf_raal_cfg_t m_config;

/**@brief Defines if RAAL is in continuous mode. */
static volatile bool m_continuous = false;

/**@brief Defines if RAAL is currently in a timeslot. */
static volatile timeslot_state_t m_timeslot_state;

/**@brief Defines if SoftDevice session is currently idle. */
static volatile bool m_session_idle;

/**@brief Current action of the timer. */
static timer_action_t m_timer_action;

/**@brief Current timeslot length. */
static uint16_t m_timeslot_length;

/**@brief Previously granted timeslot length.
 *
 * @note  The RAAL timer uses this value to fire by the end of the previously granted timeslot
 *        to attempt timeslot extension. After successful extension, the value is updated.
 */
static uint16_t m_prev_timeslot_length;

/**@brief Number of already performed extentions tries on failed event. */
static volatile uint16_t m_timeslot_extend_tries;

/***************************************************************************************************
 * @section Drift calculations
 **************************************************************************************************/

static uint32_t time_corrected_for_drift_get(uint32_t time)
{
    uint32_t ppm = m_config.sd_cfg.lf_clk_accuracy_ppm + MAX_HFCLK_PPM;

    return time - NRF_802154_DIVIDE_AND_CEIL(time * ppm, PPM_UNIT);
}

/***************************************************************************************************
 * @section Operations on RAAL TIMER.
 **************************************************************************************************/

/**@brief Set timer on timeslot started. */
static void timer_start(void)
{
    m_timer_action = TIMER_ACTION_EXTEND;
    nrf_timer_task_trigger(RAAL_TIMER, NRF_TIMER_TASK_STOP);
    nrf_timer_task_trigger(RAAL_TIMER, NRF_TIMER_TASK_CLEAR);
    nrf_timer_bit_width_set(RAAL_TIMER, NRF_TIMER_BIT_WIDTH_32);
    nrf_timer_cc_write(RAAL_TIMER, TIMER_CC_ACTION, 0);

    nrf_timer_task_trigger(RAAL_TIMER, NRF_TIMER_TASK_START);
    NVIC_EnableIRQ(RAAL_TIMER_IRQn);
}

/**@brief Reset timer. */
static void timer_reset(void)
{
    NVIC_DisableIRQ(RAAL_TIMER_IRQn);
    __DSB();
    __ISB();

    nrf_timer_task_trigger(RAAL_TIMER, NRF_TIMER_TASK_STOP);
    nrf_timer_event_clear(RAAL_TIMER, TIMER_CC_ACTION_EVENT);
}

/**@brief Get current time on RAAL Timer. */
static inline uint32_t timer_time_get(void)
{
    nrf_timer_task_trigger(RAAL_TIMER, TIMER_CC_CAPTURE_TASK);
    return nrf_timer_cc_read(RAAL_TIMER, TIMER_CC_CAPTURE);
}

/**@brief Check if timer is set to margin.
 *
 * @retval true   Timer action CC is set to the margin action.
 * @retval false  Timer action CC is set to the extend action.
 */
static inline bool timer_is_set_to_margin(void)
{
    return m_timer_action == TIMER_ACTION_MARGIN;
}

/**@brief Get the number of RTC ticks remaining to the current timeslot end. */
static inline uint32_t ticks_to_timeslot_end_get(void)
{
    uint32_t cc      = NRF_RTC0->CC[1];
    uint32_t counter = NRF_RTC0->COUNTER;

    // We add one tick as RTC might be just about to increment COUNTER value.
    return (cc - (counter + 1)) & RTC_COUNTER_COUNTER_Msk;
}

/**@brief Get the time to timeslot end in microseconds, taking into account safety margin and drifts. */
static inline uint32_t safe_time_to_timeslot_end_get(void)
{
    uint32_t margin       = m_config.sd_cfg.timeslot_safe_margin + NRF_RADIO_START_JITTER_US;
    uint32_t timeslot_end = NRF_802154_RTC_TICKS_TO_US(ticks_to_timeslot_end_get());

    if (timeslot_end > margin)
    {
        return timeslot_end - margin;
    }
    else
    {
        return 0;
    }
}

/**@brief Get timeslot margin. */
static uint32_t timer_get_cc_margin(void)
{
    uint32_t corrected_time_to_margin = time_corrected_for_drift_get(
        safe_time_to_timeslot_end_get());

    return timer_time_get() + corrected_time_to_margin;
}

/**@brief Set timer action to the timeslot margin. */
static inline void timer_to_margin_set(void)
{
    uint32_t margin_cc = timer_get_cc_margin();

    m_timer_action = TIMER_ACTION_MARGIN;

    nrf_timer_event_clear(RAAL_TIMER, TIMER_CC_ACTION_EVENT);
    nrf_timer_cc_write(RAAL_TIMER, TIMER_CC_ACTION, margin_cc);
    nrf_timer_int_enable(RAAL_TIMER, TIMER_CC_ACTION_INT);
}

/**@brief Check if margin is already reached. */
static inline bool timer_is_margin_reached(void)
{
    return timer_is_set_to_margin() && nrf_timer_event_check(RAAL_TIMER, TIMER_CC_ACTION_EVENT) &&
           safe_time_to_timeslot_end_get() <= MIN_TIME_PRIOR_MARGIN_IS_REACHED_US;
}

/**@brief Set timer on extend event. */
static void timer_on_extend_update(void)
{
    NVIC_ClearPendingIRQ(RAAL_TIMER_IRQn);

    if (timer_is_set_to_margin())
    {
        uint32_t margin_cc = nrf_timer_cc_read(RAAL_TIMER, TIMER_CC_ACTION);

        margin_cc += m_timeslot_length;
        nrf_timer_cc_write(RAAL_TIMER, TIMER_CC_ACTION, margin_cc);
    }
    else
    {
        uint32_t extension_interval = m_config.extension_interval;

        // If the granted timeslot finishes earlier than the default configuration expects,
        // set the timer for the moment it's going to end.
        if (m_prev_timeslot_length != m_config.sd_cfg.timeslot_length)
        {
            extension_interval = time_corrected_for_drift_get(m_prev_timeslot_length);
        }

        nrf_timer_cc_write(RAAL_TIMER, TIMER_CC_ACTION,
                           nrf_timer_cc_read(RAAL_TIMER, TIMER_CC_ACTION) + extension_interval);
        nrf_timer_int_enable(RAAL_TIMER, TIMER_CC_ACTION_INT);
    }
}

/***************************************************************************************************
 * @section Timeslot related functions.
 **************************************************************************************************/

/**@brief Initialize timeslot internal variables. */
static inline void timeslot_data_init(void)
{
    m_timeslot_extend_tries = 0;
    m_timeslot_length       = m_config.sd_cfg.timeslot_length;
}

/**@brief Set timeslot state to the provided state. */
static inline void timeslot_state_set(timeslot_state_t state)
{
    m_timeslot_state = state;
    __DMB();
}

/**@brief Indicate if timeslot is in the provided state. */
static inline bool timeslot_state_is(timeslot_state_t state)
{
    return (m_timeslot_state == state);
}

/**@brief Atomically check if timeslot can be requested safely. */
static bool timeslot_can_be_requested(void)
{
    bool                            result = false;
    nrf_802154_mcu_critical_state_t mcu_cs;

    nrf_802154_mcu_critical_enter(mcu_cs);

    if (m_continuous && m_session_idle && timeslot_state_is(TIMESLOT_STATE_IDLE))
    {
        // Continuous mode is on, the session and timeslot are idle. Allow for timeslot request
        timeslot_state_set(TIMESLOT_STATE_REQUESTED);
        result = true;
    }

    nrf_802154_mcu_critical_exit(mcu_cs);

    return result;
}

/**@brief Notify driver that timeslot has been started. */
static inline void timeslot_started_notify(void)
{
    if (timeslot_state_is(TIMESLOT_STATE_GRANTED) && m_continuous)
    {
        nrf_raal_timeslot_started();
    }
}

/**@brief Notify driver that timeslot has been ended. */
static inline void margin_reached_notify(void)
{
    if (!timeslot_state_is(TIMESLOT_STATE_GRANTED) && m_continuous)
    {
        nrf_raal_timeslot_ended();
    }
}

/**@brief Prepare earliest timeslot request. */
static void timeslot_request_prepare(nrf_radio_request_t * p_request)
{
    memset(p_request, 0, sizeof(nrf_radio_request_t));
    p_request->request_type               = NRF_RADIO_REQ_TYPE_EARLIEST;
    p_request->params.earliest.hfclk      = NRF_RADIO_HFCLK_CFG_NO_GUARANTEE;
    p_request->params.earliest.priority   = NRF_RADIO_PRIORITY_NORMAL;
    p_request->params.earliest.length_us  = m_timeslot_length;
    p_request->params.earliest.timeout_us = m_config.sd_cfg.timeslot_timeout;
}

/**@brief Request earliest timeslot. */
static void timeslot_request(void)
{
    nrf_radio_request_t radio_request;
    uint32_t            err_code;

    timeslot_request_prepare(&radio_request);

    // Request timeslot from SoftDevice.
    err_code = sd_radio_request(&radio_request);

    if (err_code != NRF_SUCCESS)
    {
        timeslot_state_set(TIMESLOT_STATE_IDLE);
    }

    nrf_802154_log_local_event(NRF_802154_LOG_VERBOSITY_LOW,
                               NRF_802154_LOG_LOCAL_EVENT_ID_RAAL_TIMESLOT_REQUEST,
                               radio_request.params.earliest.length_us);
    nrf_802154_log_local_event(NRF_802154_LOG_VERBOSITY_LOW,
                               NRF_802154_LOG_LOCAL_EVENT_ID_RAAL_TIMESLOT_REQUEST_RESULT,
                               err_code);
}

/**@brief Decrease timeslot length. */
static void timeslot_length_decrease(void)
{
    m_timeslot_extend_tries++;
    m_timeslot_length = m_timeslot_length >> 1;
}

/**@brief Fill timeslot parameters with extend action. */
static void timeslot_extend(uint32_t timeslot_length)
{
    m_ret_param.callback_action         = NRF_RADIO_SIGNAL_CALLBACK_ACTION_EXTEND;
    m_ret_param.params.extend.length_us = timeslot_length;

    nrf_802154_pin_set(PIN_DBG_TIMESLOT_EXTEND_REQ);
    nrf_802154_log_local_event(NRF_802154_LOG_VERBOSITY_LOW,
                               NRF_802154_LOG_LOCAL_EVENT_ID_RAAL_TIMESLOT_REQUEST,
                               m_ret_param.params.extend.length_us);
}

/**@brief Extend timeslot further. */
static void timeslot_next_extend(void)
{
    // Check if we can make another extend query.
    if (m_timeslot_extend_tries < m_config.sd_cfg.timeslot_alloc_iters)
    {
        // Decrease timeslot length.
        timeslot_length_decrease();

        // Try to extend right after start.
        timeslot_extend(m_timeslot_length);
    }
}

/***************************************************************************************************
 * @section RAAL TIMER interrupt handler and its helpers.
 **************************************************************************************************/

/**@brief Handle reached safety margin. */
static void safety_margin_exceeded_handle(void)
{
    nrf_802154_pin_clr(PIN_DBG_TIMESLOT_ACTIVE);
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    // Timeslot has been revoked. Notify higher layer
    timeslot_state_set(TIMESLOT_STATE_IDLE);
    margin_reached_notify();

    // Ignore any other events.
    timer_reset();

    // Return and wait for NRF_EVT_RADIO_SESSION_IDLE event.
    m_ret_param.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE;

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Recalculate safety margin to compensate for clock drifts. */
static void safety_margin_drift_correct(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    // Recalculate safety margin to suppress clock drifts
    timer_to_margin_set();

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle reached extension margin. */
static void extension_margin_exceeded_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    // Stop worrying about timer for a moment
    nrf_timer_int_disable(RAAL_TIMER, TIMER_CC_ACTION_INT);
    nrf_timer_event_clear(RAAL_TIMER, TIMER_CC_ACTION_EVENT);

    uint32_t elapsed_timeslot       = nrf_timer_cc_read(RAAL_TIMER, TIMER_CC_ACTION);
    bool     has_remaining_timeslot =
        m_config.sd_cfg.timeslot_max_length > (elapsed_timeslot + m_config.sd_cfg.timeslot_length);

    // Check if there's any point in trying to extend the timeslot
    if (m_continuous && has_remaining_timeslot)
    {
        // There still is some remaining timeslot that can be utilized. Try to extend it
        timeslot_extend(m_config.sd_cfg.timeslot_length);
    }
    else
    {
        // Maximum timeslot length has been reached. Set timer for safety margin
        timer_to_margin_set();

        m_ret_param.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE;
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle timer interrupts. */
static void timer_irq_handle(void)
{
    // Check that margin or extend event triggered.
    if (nrf_timer_event_check(RAAL_TIMER, TIMER_CC_ACTION_EVENT))
    {
        // Check if it was safety or extension margin that triggered the interrupt
        if (timer_is_set_to_margin())
        {
            // Timer has already been set to the safety margin. Check if it was reached
            if (timer_is_margin_reached())
            {
                // Timer has reached the safety margin. Handle it
                safety_margin_exceeded_handle();
            }
            else
            {
                // Timer has not reached the safety margin yet. Take the opportunity to correct it
                safety_margin_drift_correct();
            }
        }
        else
        {
            // Timer has not been set to the safety margin yet. The extension margin was exceeded
            extension_margin_exceeded_handle();
        }
    }
    else
    {
        // Should not happen.
        assert(false);
    }
}

/***************************************************************************************************
 * @section SoftDevice signal and SoC handlers and their helpers.
 **************************************************************************************************/

/**@brief Drop timeslot. */
static void timeslot_dropped_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    // Drop the timeslot
    timeslot_state_set(TIMESLOT_STATE_IDLE);

    // Select the appropriate action to perform based on the SoftDevice version
    if (m_config.timeslot_releasing_supported)
    {
        m_ret_param.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_END;
    }
    else
    {
        m_ret_param.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE;
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle timeslot start. */
static void timeslot_started_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    assert(timeslot_state_is(TIMESLOT_STATE_REQUESTED));

    // Session is not idle anymore
    m_session_idle = false;

    // First, set up a timer to fire immediately after leaving the signal handler
    timer_start();

    // Latch the previous timeslot length
    m_prev_timeslot_length = m_timeslot_length;

    // Reinitialize timeslot data for future timeslot extensions
    timeslot_data_init();

    // Try to extend right after start
    timeslot_extend(m_timeslot_length);

    // Do not notify started timeslot here. Notify after successful extend to make sure
    // enough timeslot length is available before notification.

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle an unwanted Radio IRQ. */
static void unwanted_radio_irq_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    // Make sure no more interrupts are going to occur and drop the current one
    NVIC_DisableIRQ(RADIO_IRQn);

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle Radio IRQs. */
static void radio_irq_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    // Verify if the interrupt should be processed at all
    if (timeslot_state_is(TIMESLOT_STATE_GRANTED))
    {
        // We are willing to process this interrupt. Check if it can be done now
        if (!timer_is_margin_reached())
        {
            // The safety margin has not been reached yet. The interrupt can be processed safely
            nrf_802154_radio_irq_handler();
        }
        else
        {
            // The safety margin has been reached and the timer interrupt cannot be delayed any longer.
            // The margin must be handled immediately and the radio interrupt should be dropped
            timer_irq_handle();
        }
    }
    else
    {
        // Timeslot is being dropped or revoked. This interrupt is obsolete
        unwanted_radio_irq_handle();
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle failed attempt to extend current timeslot. */
static void timeslot_extend_failed_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    if (!timer_is_set_to_margin())
    {
        // The timeslot is going to be revoked soon and we've just found out about it
        timer_to_margin_set();
    }

    // Try to extend if possible
    timeslot_next_extend();

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle successfully extended timeslot. */
static void timeslot_extend_succeeded_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    bool timeslot_can_be_extended = (ticks_to_timeslot_end_get() >=
                                     MINIMUM_TIMESLOT_LENGTH_EXTENSION_TIME_TICKS);

    // Verify that another timeslot extension can be attempted safely
    if (!timer_is_set_to_margin() && (!timeslot_can_be_extended))
    {
        // Timer is not set to margin yet, but there is not enough time for even the shortest
        // extension. Do not attempt extension, set timer for safety margin immediately instead
        timer_to_margin_set();

        m_ret_param.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE;
    }
    else
    {
        // Extension can be attempted safely. Set timer for the next extension
        timer_on_extend_update();

        // Update the value for which the next extension timer should be set
        m_prev_timeslot_length = m_timeslot_length;

        // Request further extension only if any of previous one failed.
        if (m_timeslot_extend_tries != 0)
        {
            timeslot_next_extend();
        }
    }

    if (timeslot_state_is(TIMESLOT_STATE_REQUESTED))
    {
        // Timeslot has been started and extended successfully. Notify the higher layer
        timeslot_state_set(TIMESLOT_STATE_GRANTED);
        timeslot_started_notify();
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle unavailable timeslot. */
static void timeslot_busy_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    assert(!timeslot_state_is(TIMESLOT_STATE_GRANTED));

    timeslot_state_set(TIMESLOT_STATE_IDLE);

    m_session_idle = true;

    if (timeslot_can_be_requested())
    {
        timeslot_request();
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

/**@brief Handle timeslot that is free to be requested. */
static void timeslot_available_handle(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    m_session_idle = true;

    if (timeslot_can_be_requested())
    {
        timeslot_data_init();
        timeslot_request();
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);

}

/**@brief Signal handler. */
static nrf_radio_signal_callback_return_param_t * signal_handler(uint8_t signal_type)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    // Default response.
    m_ret_param.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE;

    if (!m_continuous)
    {
        nrf_802154_pin_clr(PIN_DBG_TIMESLOT_ACTIVE);
        timeslot_dropped_handle();
    }
    else
    {
        switch (signal_type)
        {
            case NRF_RADIO_CALLBACK_SIGNAL_TYPE_START: /**< This signal indicates the start of the radio timeslot. */
                nrf_802154_pin_set(PIN_DBG_TIMESLOT_ACTIVE);
                timeslot_started_handle();
                break;

            case NRF_RADIO_CALLBACK_SIGNAL_TYPE_TIMER0: /**< This signal indicates the TIMER0 interrupt. */
                timer_irq_handle();
                break;

            case NRF_RADIO_CALLBACK_SIGNAL_TYPE_RADIO: /**< This signal indicates the NRF_RADIO interrupt. */
                nrf_802154_pin_set(PIN_DBG_TIMESLOT_RADIO_IRQ);
                radio_irq_handle();
                nrf_802154_pin_clr(PIN_DBG_TIMESLOT_RADIO_IRQ);
                break;

            case NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_FAILED: /**< This signal indicates extend action failed. */
                nrf_802154_pin_tgl(PIN_DBG_TIMESLOT_FAILED);
                timeslot_extend_failed_handle();
                break;

            case NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_SUCCEEDED: /**< This signal indicates extend action succeeded. */
                timeslot_extend_succeeded_handle();
                break;

            default:
                break;
        }
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);

    return &m_ret_param;
}

void nrf_raal_softdevice_soc_evt_handler(uint32_t evt_id)
{
    switch (evt_id)
    {
        case NRF_EVT_RADIO_BLOCKED:
        case NRF_EVT_RADIO_CANCELED:
            nrf_802154_pin_tgl(PIN_DBG_TIMESLOT_BLOCKED);
            timeslot_busy_handle();
            break;

        case NRF_EVT_RADIO_SESSION_IDLE:
            nrf_802154_pin_tgl(PIN_DBG_TIMESLOT_SESSION_IDLE);
            timeslot_available_handle();
            break;

        case NRF_EVT_RADIO_SIGNAL_CALLBACK_INVALID_RETURN:
            assert(false);
            break;

        case NRF_EVT_RADIO_SESSION_CLOSED:
            // Intentionally empty
            break;

        default:
            break;
    }
}

/***************************************************************************************************
 * @section RAAL API.
 **************************************************************************************************/

void nrf_raal_softdevice_config(const nrf_raal_softdevice_cfg_t * p_cfg)
{
    assert(m_initialized);
    assert(!m_continuous);
    assert(p_cfg);

    m_config.sd_cfg = *p_cfg;

    m_config.extension_interval = time_corrected_for_drift_get(m_config.sd_cfg.timeslot_length);
}

void nrf_raal_init(void)
{
    assert(!m_initialized);

    m_continuous     = false;
    m_timeslot_state = TIMESLOT_STATE_IDLE;

    m_config.sd_cfg.timeslot_length      = NRF_RAAL_TIMESLOT_DEFAULT_LENGTH;
    m_config.sd_cfg.timeslot_alloc_iters = NRF_RAAL_TIMESLOT_DEFAULT_ALLOC_ITERS;
    m_config.sd_cfg.timeslot_safe_margin = NRF_RAAL_TIMESLOT_DEFAULT_SAFE_MARGIN;
    m_config.sd_cfg.timeslot_max_length  = NRF_RAAL_TIMESLOT_DEFAULT_MAX_LENGTH;
    m_config.sd_cfg.timeslot_timeout     = NRF_RAAL_TIMESLOT_DEFAULT_TIMEOUT;
    m_config.sd_cfg.lf_clk_accuracy_ppm  = NRF_RAAL_DEFAULT_LF_CLK_ACCURACY_PPM;

    m_config.extension_interval = time_corrected_for_drift_get(m_config.sd_cfg.timeslot_length);

    uint32_t err_code = sd_radio_session_open(signal_handler);

    assert(err_code == NRF_SUCCESS);
    (void)err_code;

    m_session_idle = true;

#if (SD_VERSION == BLE_ADV_SCHED_CFG_SUPPORT_SD_VERSION)
    // Ensure that correct SoftDevice version is flashed.
    if (SD_VERSION_GET(MBR_SIZE) == BLE_ADV_SCHED_CFG_SUPPORT_SD_VERSION)
    {
        // Use improved Advertiser Role Scheduling configuration.
        ble_opt_t opt;

        memset(&opt, 0, sizeof(opt));
        opt.common_opt.adv_sched_cfg.sched_cfg = ADV_SCHED_CFG_IMPROVED;

        err_code = sd_ble_opt_set(BLE_COMMON_OPT_ADV_SCHED_CFG, &opt);

        assert(err_code == NRF_SUCCESS);
        (void)err_code;
    }
#endif

    // Ensure that correct SoftDevice version is flashed.
    if (SD_VERSION_GET(MBR_SIZE) >= TIMESLOT_RELEASE_SUPPORT_MIN_SD_VERSION)
    {
        m_config.timeslot_releasing_supported = true;
    }

    m_initialized = true;
}

void nrf_raal_uninit(void)
{
    assert(m_initialized);

    uint32_t err_code = sd_radio_session_close();

    assert(err_code == NRF_SUCCESS);
    (void)err_code;

    m_continuous     = false;
    m_timeslot_state = TIMESLOT_STATE_IDLE;

    nrf_802154_pin_clr(PIN_DBG_TIMESLOT_ACTIVE);
}

void nrf_raal_continuous_mode_enter(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    assert(m_initialized);
    assert(!m_continuous);

    m_continuous = true;

    if (timeslot_can_be_requested())
    {
        timeslot_data_init();
        timeslot_request();
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

void nrf_raal_continuous_mode_exit(void)
{
    nrf_802154_log_function_enter(NRF_802154_LOG_VERBOSITY_LOW);

    assert(m_initialized);
    assert(m_continuous);

    if (timeslot_state_is(TIMESLOT_STATE_GRANTED))
    {
        // Reset timer prior marking exiting continuous mode to prevent timeslot release caused by
        // the timer
        timer_reset();

        // Be cautious - most likely this is not an interrupt context
        timeslot_state_set(TIMESLOT_STATE_DROPPED);

        nrf_raal_timeslot_ended();
    }
    else
    {
        m_continuous = false;
    }

    nrf_802154_log_function_exit(NRF_802154_LOG_VERBOSITY_LOW);
}

void nrf_raal_continuous_ended(void)
{
    // This function should only be called at the end of a sequence of passing over the timeslot.
    // In the current implementation, the sequence might occur either due to the timeslot being
    // revoked by the Radio arbiter, in which case the timeslot is already IDLE when this function
    // is called, or as a result of a higher layer request to intentionally drop the timeslot,
    // which would make the timeslot DROPPED at the moment of this function's execution.

    // However, the timeslot state is not asserted here. It might happen that a precondition other
    // than RAAL revokes access to the RADIO peripheral, which would result in core being notified
    // about approved preconditions priority equal IDLE. In that case, this function could be called
    // with the timeslot being in some other state and it should not be considered an error.
    // It is not possible in the exising implementation, but the design allows it, so the timeslot
    // state should not be asserted here.

    if (timeslot_state_is(TIMESLOT_STATE_DROPPED))
    {
        // Timeslot was dropped intentionally
        m_continuous = false;
        __DMB();

        // Emulate signal interrupt to inform SD about end of continuous mode.
        NVIC_SetPendingIRQ(RADIO_IRQn);
        NVIC_EnableIRQ(RADIO_IRQn);
    }
    else
    {
        // Nothing to do here. Drop the notification
    }
}

bool nrf_raal_timeslot_request(uint32_t length_us)
{
    uint32_t us_left;

    if (!m_continuous || !timeslot_state_is(TIMESLOT_STATE_GRANTED))
    {
        return false;
    }

    us_left = nrf_raal_timeslot_us_left_get();

    assert((us_left >= nrf_802154_rx_duration_get(MAX_PACKET_SIZE,
                                                  true)) || timer_is_set_to_margin());

    return length_us < us_left;
}

uint32_t nrf_raal_timeslot_us_left_get(void)
{
    return timeslot_state_is(TIMESLOT_STATE_GRANTED) ? safe_time_to_timeslot_end_get() : 0;
}
