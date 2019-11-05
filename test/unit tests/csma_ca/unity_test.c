/* Copyright (c) 2019, Nordic Semiconductor ASA
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

#include "unity.h"

#include "nrf_802154_config.h"
#include "nrf_802154_const.h"
#include "mock_nrf_radio.h"
#include "mock_nrf_802154.h"
#include "mock_nrf_802154_ack_generator.h"
#include "mock_nrf_802154_core_hooks.h"
#include "mock_nrf_802154_critical_section.h"
#include "mock_nrf_802154_debug.h"
#include "mock_nrf_802154_filter.h"
#include "mock_nrf_802154_frame_parser.h"
#include "mock_nrf_802154_notification.h"
#include "mock_nrf_802154_pib.h"
#include "mock_nrf_802154_priority_drop.h"
#include "mock_nrf_802154_procedures_duration.h"
#include "mock_nrf_802154_random.h"
#include "mock_nrf_802154_request.h"
#include "mock_nrf_802154_rsch.h"
#include "mock_nrf_802154_rsch_crit_sect.h"
#include "mock_nrf_802154_rssi.h"
#include "mock_nrf_802154_rx_buffer.h"
#include "mock_nrf_802154_timer_coord.h"
#include "mock_nrf_802154_timer_sched.h"

#include "mac_features/nrf_802154_csma_ca.c"

#ifdef NRF_802154_CSMA_CA_ENABLED
    #undef NRF_802154_CSMA_CA_ENABLED
    #define NRF_802154_CSMA_CA_ENABLED 1
#endif

#define CSMACA_MIN_BE_MINIMUM       0  ///< The minimum value of the backoff exponent (BE) allowed by the protocol specification

#define CSMACA_MAX_BE_MINIMUM       3  ///< The minimum value of the maximum value of the backoff exponent (BE) allowed by the protocol specification
#define CSMACA_MAX_BE_MAXIMUM       8  ///< The maximum value of the backoff exponent (BE) allowed by the protocol specification

#define CSMACA_MAX_BACKOFFS_MINIMUM 0  ///< The minimum value of the maximum number of CSMA-CA backoffs allowed by the protocol specification
#define CSMACA_MAX_BACKOFFS_MAXIMUM 5  ///< The maximum number of the CSMA-CA backoffs allowed by the protocol specification

#define TEST_FRAME_LENGTH           16 ///< Length of the test frame in bytes.
#define COEX_TX_REQUEST_MODE_NUM    4  ///< Number of Coex TX request modes

static const uint8_t mp_test_data[TEST_FRAME_LENGTH] =
{
    0xf, 0xe, 0xd, 0xc, 0xb, 0xa, 0x9, 0x8, 0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0x0
};

static const nrf_802154_coex_tx_request_mode_t m_tx_req_modes[COEX_TX_REQUEST_MODE_NUM] =
{
    NRF_802154_COEX_TX_REQUEST_MODE_DISABLED,
    NRF_802154_COEX_TX_REQUEST_MODE_FRAME_READY,
    NRF_802154_COEX_TX_REQUEST_MODE_CCA_START,
    NRF_802154_COEX_TX_REQUEST_MODE_CCA_DONE
};

typedef struct
{
    uint8_t                           min_be;
    uint8_t                           max_be;
    uint8_t                           max_backoffs;
    nrf_802154_coex_tx_request_mode_t mode;
} test_parameters_t;

static test_parameters_t m_test_params;

/***********************************************************************************/
/***********************************************************************************/
/***********************************************************************************/

// Necessary for testing in iterative manner to avoid CMock running out of memory.
// Implementation of this function is generated automatically by Unity in testunity_runner.c
extern void resetTest(void);

void setUp(void)
{

}

void tearDown(void)
{
    m_be         = 0;
    m_nb         = 0;
    m_is_running = false;

    m_test_params.min_be       = 0;
    m_test_params.max_be       = 0;
    m_test_params.max_backoffs = 0;
    m_test_params.mode         = 0;
}

static void prepare_test_parameters(uint8_t min_be, uint8_t max_be, uint8_t max_backoffs, nrf_802154_coex_tx_request_mode_t mode)
{
    m_test_params.min_be       = min_be;
    m_test_params.max_be       = max_be;
    m_test_params.max_backoffs = max_backoffs;
    m_test_params.mode         = mode;
}

static void mock_random_backoff_start(void)
{
    uint32_t random_backoff = rand();
    uint32_t current_time   = rand();

    nrf_802154_random_get_ExpectAndReturn(random_backoff);
    nrf_802154_pib_csmaca_max_backoffs_get_ExpectAndReturn(m_test_params.max_backoffs);
    nrf_802154_timer_sched_time_get_ExpectAndReturn(current_time);
    nrf_802154_pib_coex_tx_request_mode_get_ExpectAndReturn(m_test_params.mode);
    nrf_802154_rsch_delayed_timeslot_request_IgnoreAndReturn(true);
}

static void mock_busy_and_rescheduled_backoffs(uint8_t cca_busy_backoffs)
{
    for (uint8_t backoffs = 0; backoffs < cca_busy_backoffs; backoffs++)
    {
        nrf_802154_pib_csmaca_max_be_get_ExpectAndReturn(m_test_params.max_be);
        nrf_802154_pib_csmaca_max_backoffs_get_ExpectAndReturn(m_test_params.max_backoffs);
        mock_random_backoff_start();

        nrf_802154_csma_ca_tx_failed_hook(mp_test_data, NRF_802154_TX_ERROR_BUSY_CHANNEL);

        TEST_ASSERT_TRUE((m_be >= m_test_params.min_be) && (m_be <= m_test_params.max_be));
        TEST_ASSERT_EQUAL_UINT8(m_nb, backoffs + 1);
        TEST_ASSERT_TRUE(m_is_running);
    }
}

static void mock_last_backoff_failed(void)
{
    nrf_802154_pib_csmaca_max_be_get_ExpectAndReturn(m_test_params.max_be);
    nrf_802154_pib_csmaca_max_backoffs_get_ExpectAndReturn(m_test_params.max_backoffs);
    nrf_802154_rsch_delayed_timeslot_cancel_IgnoreAndReturn(true);
    nrf_802154_csma_ca_tx_failed_hook(mp_test_data, NRF_802154_TX_ERROR_BUSY_CHANNEL);
}

static void mock_csma_ca_start(void)
{
    nrf_802154_pib_csmaca_min_be_get_ExpectAndReturn(m_test_params.min_be);
    mock_random_backoff_start();
    nrf_802154_csma_ca_start(mp_test_data);
}

static void mock_successful_backoff(void)
{
    nrf_802154_rsch_delayed_timeslot_cancel_IgnoreAndReturn(true);
    nrf_802154_csma_ca_tx_started_hook(mp_test_data);
}

static void verify_csma_ca_failed(void)
{
    // If no backoffs are allowed, verify that transmission was attempted once
    if (0 == m_test_params.max_backoffs)
    {
        uint8_t expected_be = m_test_params.max_be > m_test_params.min_be ? (m_test_params.min_be + 1) : m_test_params.max_be;

        TEST_ASSERT_EQUAL_UINT8(m_nb, 1);
        TEST_ASSERT_EQUAL_UINT8(m_be, expected_be);
    }
    // Otherwise check that the backoff was performed exactly max_backoffs times
    else
    {
        TEST_ASSERT_TRUE((m_be >= m_test_params.min_be) && (m_be <= m_test_params.max_be));
        TEST_ASSERT_EQUAL_UINT8(m_nb, m_test_params.max_backoffs);
    }

    // Verify that the procedure was stopped.
    TEST_ASSERT_FALSE(m_is_running);
}

static void verify_csma_ca_succeeded(uint8_t cca_busy_backoffs)
{
    if (0 == cca_busy_backoffs)
    {
        TEST_ASSERT_EQUAL_UINT8(m_be, m_test_params.min_be);
    }

    TEST_ASSERT_TRUE((m_be >= m_test_params.min_be) && (m_be <= m_test_params.max_be));
    TEST_ASSERT_EQUAL_UINT8(m_nb, cca_busy_backoffs);
    TEST_ASSERT_FALSE(m_is_running);
}


static void verify_channel_busy_scenario(uint8_t min_be, uint8_t max_be)
{
    for (int i = 0; i < COEX_TX_REQUEST_MODE_NUM; i++)
    {
        for (uint8_t max_backoffs = CSMACA_MAX_BACKOFFS_MINIMUM; max_backoffs <= CSMACA_MAX_BACKOFFS_MAXIMUM; max_backoffs++)
        {
            // Prepare test parameters
            prepare_test_parameters(min_be, max_be, max_backoffs, m_tx_req_modes[i]);

            // Mocks for nrf_802154_csma_ca_start()
            mock_csma_ca_start();

            // The number of backoffs must be greater than one to perform more than the last backoff
            if (max_backoffs > 1)
            {
                // Mock failed attempts followed by a reschedule
                mock_busy_and_rescheduled_backoffs(max_backoffs - 1);
            }

            // Mock that the last backoff failed and stop the procedure
            mock_last_backoff_failed();

            // Verify that the procedure was ended gracefully
            verify_csma_ca_failed();

            // Having verified all conditions so far, start over clean to avoid excessive memory consumption.
            resetTest();
        }
    }
}

static void verify_channel_empty_scenario(uint8_t min_be, uint8_t max_be)
{
    for (int i = 0; i < COEX_TX_REQUEST_MODE_NUM; i++)
    {
        for (uint8_t max_backoffs = CSMACA_MAX_BACKOFFS_MINIMUM; max_backoffs <= CSMACA_MAX_BACKOFFS_MAXIMUM; max_backoffs++)
        {
            uint8_t cca_busy_backoffs = 0;

            do
            {
                // Prepare test parameters
                prepare_test_parameters(min_be, max_be, max_backoffs, m_tx_req_modes[i]);

                // Mocks for nrf_802154_csma_ca_start()
                mock_csma_ca_start();

                // The number of failed backoffs must be non-zero to perform more than the last backoff
                if (cca_busy_backoffs > 0)
                {
                    // Mock failed attempts followed by a reschedule
                    mock_busy_and_rescheduled_backoffs(cca_busy_backoffs);
                }

                // Mock that the next attempt was successful
                mock_successful_backoff();

                // Verify that the procedure was ended gracefully
                verify_csma_ca_succeeded(cca_busy_backoffs);

                // Having verified all conditions so far, start over clean to avoid excessive memory consumption.
                resetTest();

                cca_busy_backoffs++;

            } while (cca_busy_backoffs < max_backoffs);
        }
    }
}

/***********************************************************************************/
/***********************************************************************************/
/***********************************************************************************/

void test_ShouldNotExceedMaximumNumberOfBackoffsWhenChannelIsBusy(void)
{
    verify_channel_busy_scenario(CSMACA_MIN_BE_MINIMUM, CSMACA_MAX_BE_MINIMUM);
}

void testShouldNotRepeatBackoffWhenChannelBecomesEmpty(void)
{
    verify_channel_empty_scenario(CSMACA_MIN_BE_MINIMUM, CSMACA_MAX_BE_MINIMUM);
}

void test_ShouldNotExceedBERangeWhenChannelIsBusy(void)
{
    for (uint8_t max_be = CSMACA_MAX_BE_MINIMUM; max_be <= CSMACA_MAX_BE_MAXIMUM; max_be++)
    {
        for (uint8_t min_be = CSMACA_MIN_BE_MINIMUM; min_be <= max_be; min_be++)
        {
            verify_channel_busy_scenario(min_be, max_be);
        }
    }
}

void test_ShouldNotExceedBERangeWhenChannelBecomesEmpty(void)
{
    for (uint8_t max_be = CSMACA_MAX_BE_MINIMUM; max_be <= CSMACA_MAX_BE_MAXIMUM; max_be++)
    {
        for (uint8_t min_be = CSMACA_MIN_BE_MINIMUM; min_be <= max_be; min_be++)
        {
            verify_channel_empty_scenario(min_be, max_be);
        }
    }
}
