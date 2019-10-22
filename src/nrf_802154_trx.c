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

#include <assert.h>
#include <string.h>

#include "nrf_802154_config.h"
#include "nrf_802154_const.h"
#include "nrf_802154_types.h"
#include "nrf_802154_peripherals.h"
#include "nrf_802154_pib.h"
#include "nrf_802154_rssi.h"
#include "nrf_802154_swi.h"
#include "nrf_802154_utils.h"

#include "nrf_egu.h"
#include "nrf_radio.h"

#include "nrf_802154_procedures_duration.h"
#include "nrf_802154_critical_section.h"
#include "fem/nrf_fem_protocol_api.h"

#include "nrf_802154_trx.h"

#define EGU_EVENT                     NRF_EGU_EVENT_TRIGGERED15
#define EGU_TASK                      NRF_EGU_TASK_TRIGGER15

#define EGU_HELPER1_EVENT             NRF_EGU_EVENT_TRIGGERED3
#define EGU_HELPER1_TASK              NRF_EGU_TASK_TRIGGER3
#define EGU_HELPER1_INTMASK           NRF_EGU_INT_TRIGGERED3

#define PPI_CHGRP0                    NRF_802154_PPI_CORE_GROUP                     ///< PPI group used to disable self-disabling PPIs
#define PPI_CHGRP0_DIS_TASK           NRF_PPI_TASK_CHG0_DIS                         ///< PPI task used to disable self-disabling PPIs

#define PPI_DISABLED_EGU              NRF_802154_PPI_RADIO_DISABLED_TO_EGU          ///< PPI that connects RADIO DISABLED event with EGU task
#define PPI_EGU_RAMP_UP               NRF_802154_PPI_EGU_TO_RADIO_RAMP_UP           ///< PPI that connects EGU event with RADIO TXEN or RXEN task
#define PPI_EGU_TIMER_START           NRF_802154_PPI_EGU_TO_TIMER_START             ///< PPI that connects EGU event with TIMER START task
#define PPI_CCAIDLE_FEM               NRF_802154_PPI_RADIO_CCAIDLE_TO_FEM_GPIOTE    ///< PPI that connects RADIO CCAIDLE event with GPIOTE tasks used by FEM
#define PPI_TIMER_TX_ACK              NRF_802154_PPI_TIMER_COMPARE_TO_RADIO_TXEN    ///< PPI that connects TIMER COMPARE event with RADIO TXEN task

#if NRF_802154_DISABLE_BCC_MATCHING
#define PPI_CRCOK_DIS_PPI             NRF_802154_PPI_RADIO_CRCOK_TO_PPI_GRP_DISABLE ///< PPI that connects RADIO CRCOK event with task that disables PPI group
#define PPI_CRCERROR_CLEAR            NRF_802154_PPI_RADIO_CRCERROR_TO_TIMER_CLEAR  ///< PPI that connects RADIO CRCERROR event with TIMER CLEAR task
#define PPI_ADDRESS_COUNTER_COUNT     NRF_802154_PPI_RADIO_ADDR_TO_COUNTER_COUNT    ///< PPI that connects RADIO ADDRESS event with TIMER COUNT task
#define PPI_CRCERROR_COUNTER_CLEAR    NRF_802154_PPI_RADIO_CRCERROR_COUNTER_CLEAR   ///< PPI that connects RADIO CRCERROR event with TIMER CLEAR task

#define PPI_NO_BCC_MATCHING_USED_MASK ((1U << PPI_CRCOK_DIS_PPI) |         \
                                       (1U << PPI_CRCERROR_CLEAR) |        \
                                       (1U << PPI_ADDRESS_COUNTER_COUNT) | \
                                       (1U << PPI_CRCERROR_COUNTER_CLEAR))
#else
#define PPI_RADIO_HELPER1_EGU_HELPER1 NRF_802154_PPI_RADIO_HELPER1_TO_EGU_HELPER1 ///!< PPI that connects RADIO HELPER1 event with EGU task for HELPER1 channel

#define PPI_NO_BCC_MATCHING_USED_MASK (1 << PPI_RADIO_HELPER1_EGU_HELPER1)
#endif  // NRF_802154_DISABLE_BCC_MATCHING

/**@brief Mask of all PPI channels used directly by trx module. */
#define PPI_ALL_USED_MASK ((1U << PPI_DISABLED_EGU) |    \
                           (1U << PPI_EGU_RAMP_UP) |     \
                           (1U << PPI_EGU_TIMER_START) | \
                           (1U << PPI_CCAIDLE_FEM) |     \
                           (1U << PPI_TIMER_TX_ACK) |    \
                           PPI_NO_BCC_MATCHING_USED_MASK)

#if ((PPI_ALL_USED_MASK & NRF_802154_PPI_CHANNELS_USED_MASK) != PPI_ALL_USED_MASK)
#error Some channels in PPI_ALL_USED_MASK not found in NRF_802154_PPI_CHANNELS_USED_MASK
#endif

#if NRF_802154_DISABLE_BCC_MATCHING
#define SHORT_ADDRESS_BCSTART 0UL
#else // NRF_802154_DISABLE_BCC_MATCHING
#define SHORT_ADDRESS_BCSTART NRF_RADIO_SHORT_ADDRESS_BCSTART_MASK
#endif  // NRF_802154_DISABLE_BCC_MATCHING

/// Value set to SHORTS register when no shorts should be enabled.
#define SHORTS_IDLE           0

/// Value set to SHORTS register for RX operation.
#define SHORTS_RX             (NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK | \
                               NRF_RADIO_SHORT_END_DISABLE_MASK |       \
                               SHORT_ADDRESS_BCSTART)

#define SHORTS_RX_FREE_BUFFER (NRF_RADIO_SHORT_RXREADY_START_MASK)

#define SHORTS_TX_ACK         (NRF_RADIO_SHORT_TXREADY_START_MASK | \
                               NRF_RADIO_SHORT_PHYEND_DISABLE_MASK)

#define SHORTS_CCA_TX         (NRF_RADIO_SHORT_RXREADY_CCASTART_MASK | \
                               NRF_RADIO_SHORT_CCABUSY_DISABLE_MASK |  \
                               NRF_RADIO_SHORT_CCAIDLE_TXEN_MASK |     \
                               NRF_RADIO_SHORT_TXREADY_START_MASK |    \
                               NRF_RADIO_SHORT_PHYEND_DISABLE_MASK)

#define SHORTS_TX             (NRF_RADIO_SHORT_TXREADY_START_MASK | \
                               NRF_RADIO_SHORT_PHYEND_DISABLE_MASK)

#define SHORTS_RX_ACK         (NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK | \
                               NRF_RADIO_SHORT_END_DISABLE_MASK)

#define SHORTS_ED             (NRF_RADIO_SHORT_READY_EDSTART_MASK)

#define SHORTS_CCA            (NRF_RADIO_SHORT_RXREADY_CCASTART_MASK | \
                               NRF_RADIO_SHORT_CCABUSY_DISABLE_MASK)

#define CRC_LENGTH            2        ///< Length of CRC in 802.15.4 frames [bytes]
#define CRC_POLYNOMIAL        0x011021 ///< Polynomial used for CRC calculation in 802.15.4 frames

#define TXRU_TIME             40       ///< Transmitter ramp up time [us]
#define EVENT_LAT             23       ///< END event latency [us]

typedef enum
{
    TRX_STATE_DISABLED = 0,
    TRX_STATE_IDLE,
    TRX_STATE_GOING_IDLE,
    TRX_STATE_RXFRAME,

    /* PPIS disabled deconfigured
     * RADIO is DISABLED, RXDISABLE
     * RADIO shorts are 0
     * TIMER is running
     * FEM is going to powered or is powered depending if RADIO reached DISABLED
     */
    TRX_STATE_RXFRAME_FINISHED,

    TRX_STATE_RXACK,
    TRX_STATE_TXFRAME,
    TRX_STATE_TXACK,
    TRX_STATE_STANDALONE_CCA,
    TRX_STATE_CONTINUOUS_CARRIER,
    TRX_STATE_ENERGY_DETECTION,

    /* PPIS disabled deconfigured
     * RADIO is DISABLED, TXDISABLE, RXDISABLE
     * RADIO shorts are 0
     * TIMER is stopped
     * FEM is going to powered or is powered depending if RADIO reached DISABLED
     */
    TRX_STATE_FINISHED
} trx_state_t;

/// Common parameters for the FAL handling.
static const nrf_802154_fal_event_t m_deactivate_on_disable =
{
    .type                           = NRF_802154_FAL_EVENT_TYPE_GENERIC,
    .override_ppi                   = false,
    .event.generic.register_address =
        ((uint32_t)NRF_RADIO_BASE + (uint32_t)NRF_RADIO_EVENT_DISABLED)
};

static const nrf_802154_fal_event_t m_activate_rx_cc0 =
{
    .type                             = NRF_802154_FAL_EVENT_TYPE_TIMER,
    .override_ppi                     = false,
    .event.timer.p_timer_instance     = NRF_802154_TIMER_INSTANCE,
    .event.timer.compare_channel_mask =
        ((1 << NRF_TIMER_CC_CHANNEL0) | (1 << NRF_TIMER_CC_CHANNEL2)),
    .event.timer.counter_value = RX_RAMP_UP_TIME
};

static const nrf_802154_fal_event_t m_activate_tx_cc0 =
{
    .type                             = NRF_802154_FAL_EVENT_TYPE_TIMER,
    .override_ppi                     = false,
    .event.timer.p_timer_instance     = NRF_802154_TIMER_INSTANCE,
    .event.timer.compare_channel_mask =
        ((1 << NRF_TIMER_CC_CHANNEL0) | (1 << NRF_TIMER_CC_CHANNEL2)),
    .event.timer.counter_value = TX_RAMP_UP_TIME
};

static const nrf_802154_fal_event_t m_ccaidle =
{
    .type                           = NRF_802154_FAL_EVENT_TYPE_GENERIC,
    .override_ppi                   = true,
    .ppi_ch_id                      = PPI_CCAIDLE_FEM,
    .event.generic.register_address = ((uint32_t)NRF_RADIO_BASE + (uint32_t)NRF_RADIO_EVENT_CCAIDLE)
};

/**@brief Fal event used by @ref nrf_802154_trx_transmit_ack and @ref txack_finish */
static nrf_802154_fal_event_t m_activate_tx_cc0_timeshifted;

static volatile trx_state_t m_trx_state;

typedef struct
{
#if !NRF_802154_DISABLE_BCC_MATCHING
    bool psdu_being_received; ///< If PSDU is currently being received.

#endif

    bool missing_receive_buffer; ///!< If trx entered receive state without receive buffer

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
    bool tx_started;             ///< If the requested transmission has started.

#endif  // NRF_802154_TX_STARTED_NOTIFY_ENABLED

    bool rssi_started;
} nrf_802154_flags_t;

static nrf_802154_flags_t m_flags; ///< Flags used to store the current driver state.

/**@brief Value of TIMER internal counter from which the counting is resumed on RADIO.EVENTS_END event. */
static volatile uint32_t m_timer_value_on_radio_end_event;
static volatile bool     m_transmit_with_cca;

static void go_idle_abort(void);
static void receive_frame_abort(void);
static void receive_ack_abort(void);
static void transmit_frame_abort(void);
static void transmit_ack_abort(void);
static void standalone_cca_abort(void);
static void continuous_carrier_abort(void);
static void energy_detection_abort(void);

/** Clear flags describing frame being received. */
void rx_flags_clear(void)
{
    m_flags.missing_receive_buffer = false;
#if !NRF_802154_DISABLE_BCC_MATCHING
    m_flags.psdu_being_received = false;
#endif // !NRF_802154_DISABLE_BCC_MATCHING
}

static void * volatile mp_receive_buffer;

/** Initialize TIMER peripheral used by the driver. */
void nrf_timer_init(void)
{
    nrf_timer_mode_set(NRF_802154_TIMER_INSTANCE, NRF_TIMER_MODE_TIMER);
    nrf_timer_bit_width_set(NRF_802154_TIMER_INSTANCE, NRF_TIMER_BIT_WIDTH_16);
    nrf_timer_frequency_set(NRF_802154_TIMER_INSTANCE, NRF_TIMER_FREQ_1MHz);

#if NRF_802154_DISABLE_BCC_MATCHING
    // Setup timer for detecting PSDU reception.
    nrf_timer_mode_set(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_MODE_COUNTER);
    nrf_timer_bit_width_set(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_BIT_WIDTH_8);
#endif
}

/** Reset radio peripheral. */
static void nrf_radio_reset(void)
{
    nrf_radio_power_set(false);
    nrf_radio_power_set(true);

    nrf_802154_log(EVENT_RADIO_RESET, 0);
}

static void channel_set(uint8_t channel)
{
    assert(channel >= 11U && channel <= 26U);

    nrf_radio_frequency_set(2405U + 5U * (channel - 11U));
}

static void cca_configuration_update(void)
{
    nrf_802154_cca_cfg_t cca_cfg;

    nrf_802154_pib_cca_cfg_get(&cca_cfg);
    nrf_radio_cca_configure(cca_cfg.mode,
                            nrf_802154_rssi_cca_ed_threshold_corrected_get(cca_cfg.ed_threshold),
                            cca_cfg.corr_threshold,
                            cca_cfg.corr_limit);
}

/** Initialize interrupts for radio peripheral. */
static void irq_init(void)
{
#if !NRF_IS_IRQ_PRIORITY_ALLOWED(NRF_802154_IRQ_PRIORITY)
#error NRF_802154_IRQ_PRIORITY value out of the allowed range.
#endif
    NVIC_SetPriority(RADIO_IRQn, NRF_802154_IRQ_PRIORITY);
    NVIC_ClearPendingIRQ(RADIO_IRQn);
}

/** Wait time needed to propagate event through PPI to EGU.
 *
 * During detection if trigger of DISABLED event caused start of hardware procedure, detecting
 * function needs to wait until event is propagated from RADIO through PPI to EGU. This delay is
 * required to make sure EGU event is set if hardware was prepared before DISABLED event was
 * triggered.
 */
static inline void ppi_and_egu_delay_wait(void)
{
    __ASM("nop");
    __ASM("nop");
    __ASM("nop");
    __ASM("nop");
    __ASM("nop");
    __ASM("nop");
}

/** Detect if PPI starting EGU for current operation worked.
 *
 * @retval  true   PPI worked.
 * @retval  false  PPI did not work. DISABLED task should be triggered.
 */
static bool ppi_egu_worked(void)
{
    // Detect if PPIs were set before DISABLED event was notified. If not trigger DISABLE
    if (nrf_radio_state_get() != NRF_RADIO_STATE_DISABLED)
    {
        // If RADIO state is not DISABLED, it means that RADIO is still ramping down or already
        // started ramping up.
        return true;
    }

    // Wait for PPIs
    ppi_and_egu_delay_wait();

    if (nrf_egu_event_check(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT))
    {
        // If EGU event is set, procedure is running.
        return true;
    }
    else
    {
        return false;
    }
}

static void trigger_disable_to_start_rampup(void)
{
    if (!ppi_egu_worked())
    {
        nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);
    }
}

/** Configure FEM to set LNA at appropriate time. */
static void fem_for_lna_set(void)
{
    if (nrf_802154_fal_lna_configuration_set(&m_activate_rx_cc0, NULL) == NRF_SUCCESS)
    {
        uint32_t event_addr = (uint32_t)nrf_egu_event_address_get(NRF_802154_SWI_EGU_INSTANCE,
                                                                  EGU_EVENT);
        uint32_t task_addr = (uint32_t)nrf_timer_task_address_get(NRF_802154_TIMER_INSTANCE,
                                                                  NRF_TIMER_TASK_START);

        nrf_timer_shorts_enable(m_activate_rx_cc0.event.timer.p_timer_instance,
                                NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
        nrf_ppi_channel_endpoint_setup(PPI_EGU_TIMER_START, event_addr, task_addr);
        nrf_ppi_channel_enable(PPI_EGU_TIMER_START);
    }
}

/** Reset FEM configuration for LNA. */
static void fem_for_lna_reset(void)
{
    nrf_802154_fal_lna_configuration_clear(&m_activate_rx_cc0, NULL);
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
    nrf_ppi_channel_disable(PPI_EGU_TIMER_START);
}

/** Configure FEM to set PA at appropriate time. */
static void fem_for_pa_set(void)
{
    if (nrf_802154_fal_pa_configuration_set(&m_activate_tx_cc0, NULL) == NRF_SUCCESS)
    {
        uint32_t event_addr = (uint32_t)nrf_egu_event_address_get(NRF_802154_SWI_EGU_INSTANCE,
                                                                  EGU_EVENT);
        uint32_t task_addr = (uint32_t)nrf_timer_task_address_get(NRF_802154_TIMER_INSTANCE,
                                                                  NRF_TIMER_TASK_START);

        nrf_timer_shorts_enable(m_activate_tx_cc0.event.timer.p_timer_instance,
                                NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
        nrf_ppi_channel_endpoint_setup(PPI_EGU_TIMER_START, event_addr, task_addr);
        nrf_ppi_channel_enable(PPI_EGU_TIMER_START);
    }
}

/** Reset FEM configuration for PA. */
static void fem_for_pa_reset(void)
{
    nrf_802154_fal_pa_configuration_clear(&m_activate_tx_cc0, NULL);
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_ppi_channel_disable(PPI_EGU_TIMER_START);
    nrf_802154_fal_deactivate_now(NRF_802154_FAL_PA);
}

/** Configure FEM for TX procedure. */
static void fem_for_tx_set(bool cca)
{
    bool success;

    if (cca)
    {
        bool pa_set  = false;
        bool lna_set = false;

        if (nrf_802154_fal_lna_configuration_set(&m_activate_rx_cc0, &m_ccaidle) == NRF_SUCCESS)
        {
            lna_set = true;
        }

        if (nrf_802154_fal_pa_configuration_set(&m_ccaidle, NULL) == NRF_SUCCESS)
        {
            pa_set = true;
        }

        success = pa_set || lna_set;

    }
    else
    {
        success = (nrf_802154_fal_pa_configuration_set(&m_activate_tx_cc0, NULL) == NRF_SUCCESS);
    }

    if (success)
    {
        nrf_timer_shorts_enable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);

        uint32_t egu_event_addr = (uint32_t)nrf_egu_event_address_get(NRF_802154_SWI_EGU_INSTANCE,
                                                                      EGU_EVENT);
        uint32_t timer_task_addr = (uint32_t)nrf_timer_task_address_get(NRF_802154_TIMER_INSTANCE,
                                                                        NRF_TIMER_TASK_START);

        nrf_ppi_channel_endpoint_setup(PPI_EGU_TIMER_START, egu_event_addr, timer_task_addr);
        nrf_ppi_channel_enable(PPI_EGU_TIMER_START);
    }
}

static void fem_for_tx_reset(bool cca, bool disable_ppi_egu_timer_start)
{
    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);

    if (cca)
    {
        nrf_802154_fal_pa_configuration_clear(&m_activate_rx_cc0, &m_ccaidle);
    }
    else
    {
        nrf_802154_fal_pa_configuration_clear(&m_activate_tx_cc0, NULL);
    }

    if (disable_ppi_egu_timer_start)
    {
        nrf_ppi_channel_disable(PPI_EGU_TIMER_START);
        ppi_and_egu_delay_wait();
        nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    }
}

/** Set PPIs to connect DISABLED->EGU->RAMP_UP
 *
 * @param[in]  ramp_up_task    Task triggered to start ramp up procedure.
 * @param[in]  self_disabling  If PPI should disable itself.
 */
static void ppis_for_egu_and_ramp_up_set(nrf_radio_task_t ramp_up_task, bool self_disabling)
{
    if (self_disabling)
    {
        nrf_ppi_channel_and_fork_endpoint_setup(PPI_EGU_RAMP_UP,
                                                (uint32_t)nrf_egu_event_address_get(
                                                    NRF_802154_SWI_EGU_INSTANCE,
                                                    EGU_EVENT),
                                                (uint32_t)nrf_radio_task_address_get(ramp_up_task),
                                                (uint32_t)nrf_ppi_task_address_get(
                                                    PPI_CHGRP0_DIS_TASK));
    }
    else
    {
        nrf_ppi_channel_endpoint_setup(PPI_EGU_RAMP_UP,
                                       (uint32_t)nrf_egu_event_address_get(
                                           NRF_802154_SWI_EGU_INSTANCE,
                                           EGU_EVENT),
                                       (uint32_t)nrf_radio_task_address_get(ramp_up_task));
    }

    nrf_ppi_channel_endpoint_setup(PPI_DISABLED_EGU,
                                   (uint32_t)nrf_radio_event_address_get(NRF_RADIO_EVENT_DISABLED),
                                   (uint32_t)nrf_egu_task_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_TASK));

    if (self_disabling)
    {
        nrf_ppi_channel_include_in_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);
    }

    nrf_ppi_channel_enable(PPI_EGU_RAMP_UP);
    nrf_ppi_channel_enable(PPI_DISABLED_EGU);
}

void nrf_802154_trx_init(void)
{
    m_trx_state = TRX_STATE_DISABLED;

    nrf_timer_init();
#if defined(NRF_RADIO_EVENT_HELPER1)
    nrf_802154_swi_init();
#endif
}

void nrf_802154_trx_enable(void)
{
    assert(m_trx_state == TRX_STATE_DISABLED);

    nrf_radio_reset();

    nrf_radio_packet_conf_t packet_conf;

    nrf_radio_mode_set(NRF_RADIO_MODE_IEEE802154_250KBIT);

    memset(&packet_conf, 0, sizeof(packet_conf));
    packet_conf.lflen  = 8;
    packet_conf.plen   = NRF_RADIO_PREAMBLE_LENGTH_32BIT_ZERO;
    packet_conf.crcinc = true;
    packet_conf.maxlen = MAX_PACKET_SIZE;
    nrf_radio_packet_configure(&packet_conf);

    nrf_radio_modecnf0_set(true, 0);

    // Configure CRC
    nrf_radio_crc_configure(CRC_LENGTH, NRF_RADIO_CRC_ADDR_IEEE802154, CRC_POLYNOMIAL);

    // Configure CCA
    cca_configuration_update();

    // Set channel
    channel_set(nrf_802154_pib_channel_get());

    irq_init();

    assert(nrf_radio_shorts_get() == SHORTS_IDLE);

    nrf_802154_fal_pa_configuration_set(NULL, &m_deactivate_on_disable);
    nrf_802154_fal_lna_configuration_set(NULL, &m_deactivate_on_disable);

    nrf_802154_fal_deactivate_now(NRF_802154_FAL_ALL);

    m_trx_state = TRX_STATE_IDLE;
}

static void fem_power_down_now(void)
{
    nrf_802154_fal_deactivate_now(NRF_802154_FAL_ALL);

    if (nrf_fem_prepare_powerdown(NRF_802154_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL0,
                                  PPI_EGU_TIMER_START))
    {
        // FEM requires timer to run to perform powering down operation
        nrf_timer_event_clear(NRF_802154_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0);

        nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_START);

        while (!nrf_timer_event_check(NRF_802154_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0))
        {
            // Wait until the event is set.
            // The waiting takes several microseconds
        }

        nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
        nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
        nrf_ppi_channel_disable(PPI_EGU_TIMER_START);
    }
}

void nrf_802154_trx_disable(void)
{
    if (m_trx_state != TRX_STATE_DISABLED)
    {
        nrf_radio_power_set(false);

        /* While the RADIO is powered off deconfigure any PPIs used directly by trx module */
        nrf_ppi_channels_disable(PPI_ALL_USED_MASK);

#if !NRF_802154_DISABLE_BCC_MATCHING
        nrf_ppi_fork_endpoint_setup(PPI_EGU_RAMP_UP, 0);

#if defined(NRF_RADIO_EVENT_HELPER1)
        nrf_egu_int_disable(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_INTMASK);
#endif
#else
        nrf_ppi_fork_endpoint_setup(PPI_EGU_TIMER_START, 0);
#endif // NRF_802154_DISABLE_BCC_MATCHING

        nrf_ppi_channel_remove_from_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);

        /* Stop & deconfigure timer */
        nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE,
                                 NRF_TIMER_SHORT_COMPARE0_STOP_MASK |
                                 NRF_TIMER_SHORT_COMPARE1_STOP_MASK);
        nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

        nrf_radio_power_set(true);

        nrf_802154_fal_pa_configuration_clear(NULL, &m_deactivate_on_disable);
        nrf_802154_fal_lna_configuration_clear(NULL, &m_deactivate_on_disable);

        nrf_802154_fal_deactivate_now(NRF_802154_FAL_ALL);

        if (m_trx_state != TRX_STATE_IDLE)
        {
            fem_power_down_now();
        }

#if NRF_802154_DISABLE_BCC_MATCHING
        // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
        nrf_timer_task_trigger(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
        nrf_timer_shorts_disable(NRF_802154_COUNTER_TIMER_INSTANCE,
                                 NRF_TIMER_SHORT_COMPARE1_STOP_MASK);
#else
        m_flags.psdu_being_received = false;
#endif
        m_flags.missing_receive_buffer = false;
        m_flags.rssi_started           = false;
#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
        m_flags.tx_started = false;
#endif

        m_trx_state = TRX_STATE_DISABLED;

        nrf_802154_log(EVENT_RADIO_RESET, 0);
    }
    else
    {
        // Intentionally empty
    }
}

void nrf_802154_trx_channel_set(uint8_t channel)
{
    channel_set(channel);
}

void nrf_802154_trx_cca_configuration_update(void)
{
    cca_configuration_update();
}

/** Check if PSDU is currently being received.
 *
 * @returns True if radio is receiving PSDU, false otherwise.
 */
bool nrf_802154_trx_psdu_is_being_received(void)
{
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_timer_task_trigger(NRF_802154_COUNTER_TIMER_INSTANCE,
                           nrf_timer_capture_task_get(NRF_TIMER_CC_CHANNEL0));
    uint32_t counter = nrf_timer_cc_read(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL0);

    assert(counter <= 1);

    return counter > 0;
#else // NRF_802154_DISABLE_BCC_MATCHING
    return m_flags.psdu_being_received;
#endif  // NRF_802154_DISABLE_BCC_MATCHING
}

bool nrf_802154_trx_receive_is_buffer_missing(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_RXFRAME:
        /* no break */
        case TRX_STATE_RXACK:
            return m_flags.missing_receive_buffer;

        default:
            assert(!m_flags.missing_receive_buffer);
            return false;
    }
}

bool nrf_802154_trx_receive_buffer_set(void * p_receive_buffer)
{
    bool result = false;

    mp_receive_buffer = p_receive_buffer;

    if ((p_receive_buffer != NULL) && m_flags.missing_receive_buffer)
    {
        uint32_t shorts = SHORTS_IDLE;

        switch (m_trx_state)
        {
            case TRX_STATE_RXFRAME:
                shorts = SHORTS_RX | SHORTS_RX_FREE_BUFFER;
                break;

            case TRX_STATE_RXACK:
                shorts = SHORTS_RX_ACK | SHORTS_RX_FREE_BUFFER;
                break;

            default:
                assert(false);
        }

        m_flags.missing_receive_buffer = false;

        nrf_radio_packetptr_set(p_receive_buffer);

        nrf_radio_shorts_set(shorts);

        if (nrf_radio_state_get() == NRF_RADIO_STATE_RXIDLE)
        {
            nrf_radio_task_trigger(NRF_RADIO_TASK_START);
        }

        result = true;
    }

    return result;
}

void nrf_802154_trx_receive_frame(uint8_t                                bcc,
                                  nrf_802154_trx_receive_notifications_t notifications_mask)
{
    uint32_t ints_to_enable = 0U;
    uint32_t shorts         = SHORTS_RX;

    // Force the TIMER to be stopped and count from 0.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

    m_trx_state = TRX_STATE_RXFRAME;

    // Clear filtering flag
    rx_flags_clear();
    // Clear the RSSI measurement flag.
    m_flags.rssi_started = false;

    nrf_radio_txpower_set(nrf_802154_pib_tx_power_get());

    if (mp_receive_buffer != NULL)
    {
        m_flags.missing_receive_buffer = false;
        nrf_radio_packetptr_set(mp_receive_buffer);
        shorts |= SHORTS_RX_FREE_BUFFER;
    }
    else
    {
        m_flags.missing_receive_buffer = true;
    }

    // Set shorts
    nrf_radio_shorts_set(shorts);

    // Set BCC
#if !NRF_802154_DISABLE_BCC_MATCHING
    assert(bcc != 0U);
    nrf_radio_bcc_set(bcc * 8U);
#else
    assert(bcc == 0U);
    (void)bcc;
#endif // !NRF_802154_DISABLE_BCC_MATCHING

    // Enable IRQs
#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
    nrf_radio_event_clear(NRF_RADIO_EVENT_CRCERROR);
    ints_to_enable |= NRF_RADIO_INT_CRCERROR_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING ||NRF_802154_NOTIFY_CRCERROR
#if !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_event_clear(NRF_RADIO_EVENT_BCMATCH);
    ints_to_enable |= NRF_RADIO_INT_BCMATCH_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_event_clear(NRF_RADIO_EVENT_CRCOK);
    ints_to_enable |= NRF_RADIO_INT_CRCOK_MASK;

    if ((notifications_mask & TRX_RECEIVE_NOTIFICATION_STARTED) != 0U)
    {
        nrf_radio_event_clear(NRF_RADIO_EVENT_ADDRESS);
        ints_to_enable |= NRF_RADIO_INT_ADDRESS_MASK;
    }

    if ((notifications_mask & TRX_RECEIVE_NOTIFICATION_PRESTARTED) != 0U)
    {
#if NRF_802154_DISABLE_BCC_MATCHING || !defined(NRF_RADIO_EVENT_HELPER1)
        assert(false);
#else
        // The RADIO can't generate interrupt on EVENTS_HELPER1. Path to generate interrupt:
        // RADIO.EVENTS_HELPER1 -> PPI_RADIO_HELPER1_EGU_HELPER1 -> EGU.TASK_HELPER1 -> EGU.EVENT_HELPER1 ->
        // SWI_IRQHandler (in nrf_802154_swi.c), calls nrf_802154_trx_swi_irq_handler
        nrf_ppi_channel_endpoint_setup(PPI_RADIO_HELPER1_EGU_HELPER1,
                                       (uint32_t)nrf_radio_event_address_get(
                                           NRF_RADIO_EVENT_HELPER1),
                                       (uint32_t)nrf_egu_task_address_get(
                                           NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_TASK));
        nrf_ppi_channel_enable(PPI_RADIO_HELPER1_EGU_HELPER1);

        nrf_radio_event_clear(NRF_RADIO_EVENT_HELPER1);
        nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_EVENT);
        nrf_egu_int_enable(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_INTMASK);
#endif
    }

    nrf_radio_int_enable(ints_to_enable);

    // Set FEM
    uint32_t delta_time;

    if (nrf_802154_fal_lna_configuration_set(&m_activate_rx_cc0, NULL) == NRF_SUCCESS)
    {
        delta_time = nrf_timer_cc_read(NRF_802154_TIMER_INSTANCE,
                                       NRF_TIMER_CC_CHANNEL0);
    }
    else
    {
        delta_time = 1;
        nrf_timer_cc_write(NRF_802154_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL0, delta_time);
    }

    m_timer_value_on_radio_end_event = delta_time;

    // Let the TIMER stop on last event required by a FEM
    nrf_timer_shorts_enable(NRF_802154_TIMER_INSTANCE,
                            NRF_TIMER_SHORT_COMPARE0_STOP_MASK);

#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_timer_shorts_enable(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE1_STOP_MASK);
    nrf_timer_cc_write(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL1, 1);
#endif // NRF_802154_DISABLE_BCC_MATCHING

    // Clr event EGU (look at trigger_disable_to_start_rampup to see why)
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_endpoint_setup(PPI_EGU_RAMP_UP,
                                   (uint32_t)nrf_egu_event_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_EVENT),
                                   (uint32_t)nrf_radio_task_address_get(NRF_RADIO_TASK_RXEN));
    nrf_ppi_channel_and_fork_endpoint_setup(PPI_EGU_TIMER_START,
                                            (uint32_t)nrf_egu_event_address_get(
                                                NRF_802154_SWI_EGU_INSTANCE,
                                                EGU_EVENT),
                                            (uint32_t)nrf_timer_task_address_get(
                                                NRF_802154_TIMER_INSTANCE,
                                                NRF_TIMER_TASK_START),
                                            (uint32_t)nrf_timer_task_address_get(
                                                NRF_802154_COUNTER_TIMER_INSTANCE,
                                                NRF_TIMER_TASK_START));
    // Anomaly 78: use SHUTDOWN instead of CLEAR.
    nrf_ppi_channel_endpoint_setup(PPI_CRCERROR_CLEAR,
                                   (uint32_t)nrf_radio_event_address_get(NRF_RADIO_EVENT_CRCERROR),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_SHUTDOWN));
    nrf_ppi_channel_endpoint_setup(PPI_CRCOK_DIS_PPI,
                                   (uint32_t)nrf_radio_event_address_get(NRF_RADIO_EVENT_CRCOK),
                                   (uint32_t)nrf_ppi_task_address_get(PPI_CHGRP0_DIS_TASK));
#else // NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_and_fork_endpoint_setup(PPI_EGU_RAMP_UP,
                                            (uint32_t)nrf_egu_event_address_get(
                                                NRF_802154_SWI_EGU_INSTANCE,
                                                EGU_EVENT),
                                            (uint32_t)nrf_radio_task_address_get(
                                                NRF_RADIO_TASK_RXEN),
                                            (uint32_t)nrf_ppi_task_address_get(
                                                PPI_CHGRP0_DIS_TASK));
    nrf_ppi_channel_endpoint_setup(PPI_EGU_TIMER_START,
                                   (uint32_t)nrf_egu_event_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_EVENT),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_START));
#endif // NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_include_in_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);

    nrf_ppi_channel_endpoint_setup(PPI_DISABLED_EGU,
                                   (uint32_t)nrf_radio_event_address_get(NRF_RADIO_EVENT_DISABLED),
                                   (uint32_t)nrf_egu_task_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_TASK));
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_endpoint_setup(PPI_ADDRESS_COUNTER_COUNT,
                                   (uint32_t)nrf_radio_event_address_get(NRF_RADIO_EVENT_ADDRESS),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_COUNTER_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_COUNT));
    // Anomaly 78: use SHUTDOWN instead of CLEAR.
    nrf_ppi_channel_endpoint_setup(PPI_CRCERROR_COUNTER_CLEAR,
                                   (uint32_t)nrf_radio_event_address_get(NRF_RADIO_EVENT_CRCERROR),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_COUNTER_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_SHUTDOWN));
#endif // NRF_802154_DISABLE_BCC_MATCHING

    nrf_ppi_channel_enable(PPI_EGU_RAMP_UP);
    nrf_ppi_channel_enable(PPI_EGU_TIMER_START);
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_enable(PPI_CRCERROR_CLEAR);
    nrf_ppi_channel_enable(PPI_CRCOK_DIS_PPI);
    nrf_ppi_channel_enable(PPI_ADDRESS_COUNTER_COUNT);
    nrf_ppi_channel_enable(PPI_CRCERROR_COUNTER_CLEAR);
#endif // NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_enable(PPI_DISABLED_EGU);

    trigger_disable_to_start_rampup();
}

void nrf_802154_trx_receive_ack(void)
{
    uint32_t shorts         = SHORTS_RX_ACK;
    uint32_t ints_to_enable = 0U;

    m_trx_state = TRX_STATE_RXACK;

    if (mp_receive_buffer != NULL)
    {
        m_flags.missing_receive_buffer = false;
        nrf_radio_packetptr_set(mp_receive_buffer);
        shorts |= SHORTS_RX_FREE_BUFFER;
    }
    else
    {
        m_flags.missing_receive_buffer = true;
    }

    nrf_radio_shorts_set(shorts);

    nrf_radio_event_clear(NRF_RADIO_EVENT_ADDRESS);
    ints_to_enable |= NRF_RADIO_INT_ADDRESS_MASK;
    nrf_radio_event_clear(NRF_RADIO_EVENT_CRCOK);
    ints_to_enable |= NRF_RADIO_INT_CRCOK_MASK;
    nrf_radio_event_clear(NRF_RADIO_EVENT_CRCERROR);
    ints_to_enable |= NRF_RADIO_INT_CRCERROR_MASK;

    nrf_radio_int_enable(ints_to_enable);

    // Set PPIs necessary in rx_ack state
    fem_for_lna_set();

    nrf_ppi_channel_and_fork_endpoint_setup(PPI_EGU_RAMP_UP,
                                            (uint32_t)nrf_egu_event_address_get(
                                                NRF_802154_SWI_EGU_INSTANCE,
                                                EGU_EVENT),
                                            (uint32_t)nrf_radio_task_address_get(
                                                NRF_RADIO_TASK_RXEN),
                                            (uint32_t)nrf_ppi_task_address_get(
                                                PPI_CHGRP0_DIS_TASK));
    nrf_ppi_channel_include_in_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);

    nrf_ppi_channel_endpoint_setup(PPI_DISABLED_EGU,
                                   (uint32_t)nrf_radio_event_address_get(NRF_RADIO_EVENT_DISABLED),
                                   (uint32_t)nrf_egu_task_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_TASK));

    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Enable PPI disabled by DISABLED event
    nrf_ppi_channel_enable(PPI_EGU_RAMP_UP);

    // Enable EGU PPI to start all PPIs synchronously
    nrf_ppi_channel_enable(PPI_DISABLED_EGU);

    trigger_disable_to_start_rampup();
}

bool nrf_802154_trx_rssi_measure(void)
{
    bool result = false;

    if (m_trx_state == TRX_STATE_RXFRAME)
    {
        nrf_radio_event_clear(NRF_RADIO_EVENT_RSSIEND);
        nrf_radio_task_trigger(NRF_RADIO_TASK_RSSISTART);
        m_flags.rssi_started = true;

        result = true;
    }

    return result;
}

bool nrf_802154_trx_rssi_measure_is_started(void)
{
    return m_flags.rssi_started;
}

uint8_t nrf_802154_trx_rssi_last_sample_get(void)
{
    return nrf_radio_rssi_sample_get();
}

bool nrf_802154_trx_rssi_sample_is_available(void)
{
    return nrf_radio_event_check(NRF_RADIO_EVENT_RSSIEND);
}

void nrf_802154_trx_transmit_frame(const void * p_transmit_buffer, bool cca)
{
    uint32_t ints_to_enable = 0U;

    // Force the TIMER to be stopped and count from 0.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

    m_trx_state         = TRX_STATE_TXFRAME;
    m_transmit_with_cca = cca;

    nrf_radio_txpower_set(nrf_802154_pib_tx_power_get());
    nrf_radio_packetptr_set(p_transmit_buffer);

    // Set shorts
    nrf_radio_shorts_set(cca ? SHORTS_CCA_TX : SHORTS_TX);

    // Enable IRQs
    nrf_radio_event_clear(NRF_RADIO_EVENT_PHYEND);
    ints_to_enable |= NRF_RADIO_INT_PHYEND_MASK;

    if (cca)
    {
        nrf_radio_event_clear(NRF_RADIO_EVENT_CCABUSY);
        ints_to_enable |= NRF_RADIO_INT_CCABUSY_MASK;
    }

    nrf_radio_event_clear(NRF_RADIO_EVENT_ADDRESS);
#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
    ints_to_enable    |= NRF_RADIO_INT_ADDRESS_MASK;
    m_flags.tx_started = false;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

    nrf_radio_int_enable(ints_to_enable);

    // Set FEM
    fem_for_tx_set(cca);

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
    ppis_for_egu_and_ramp_up_set(cca ? NRF_RADIO_TASK_RXEN : NRF_RADIO_TASK_TXEN, true);

    trigger_disable_to_start_rampup();
}

bool nrf_802154_trx_transmit_ack(const void * p_transmit_buffer, uint32_t delay_us)
{
    /* Assumptions on peripherals
     * TIMER is running, is counting from value saved in m_timer_value_on_radio_end_event,
     * which trigered on END event, which happened EVENT_LAT us after frame on air receive was finished.
     * RADIO is DISABLED
     * PPIs are DISABLED
     */

    bool result = false;

    assert(m_trx_state == TRX_STATE_RXFRAME_FINISHED);
    assert(p_transmit_buffer != NULL);

    m_trx_state = TRX_STATE_TXACK;

    // Set TIMER's CC to the moment when ramp-up should occur.
    if (delay_us <= TXRU_TIME + EVENT_LAT)
    {
        nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
        return result;
    }

    uint32_t timer_cc_ramp_up_start = m_timer_value_on_radio_end_event + delay_us - TXRU_TIME -
                                      EVENT_LAT;

    nrf_timer_cc_write(NRF_802154_TIMER_INSTANCE,
                       NRF_TIMER_CC_CHANNEL1,
                       timer_cc_ramp_up_start);

    nrf_radio_packetptr_set(p_transmit_buffer);

    // Set shorts
    nrf_radio_shorts_set(SHORTS_TX_ACK);

    // Clear TXREADY event to detect if PPI worked
    nrf_radio_event_clear(NRF_RADIO_EVENT_TXREADY);

    // Set PPIs
    nrf_ppi_channel_endpoint_setup(PPI_TIMER_TX_ACK,
                                   (uint32_t)nrf_timer_event_address_get(
                                       NRF_802154_TIMER_INSTANCE,
                                       NRF_TIMER_EVENT_COMPARE1),
                                   (uint32_t)nrf_radio_task_address_get(
                                       NRF_RADIO_TASK_TXEN));

    // Set FEM PPIs
    // Note: the TIMER is running, ramp up will start in timer_cc_ramp_up_start tick
    // Assumption here is that FEM activation takes no more than TXRU_TIME.
    m_activate_tx_cc0_timeshifted = m_activate_tx_cc0;

    // Set the moment for FEM at which real transmission starts.
    m_activate_tx_cc0_timeshifted.event.timer.counter_value = timer_cc_ramp_up_start + TXRU_TIME;

    if (nrf_802154_fal_pa_configuration_set(&m_activate_tx_cc0_timeshifted, NULL) == NRF_SUCCESS)
    {
        // FEM scheduled its operations on timer, so the timer must be running until last
        // operation scheduled by the FEM (TIMER's CC0), which is later than radio ramp up
        nrf_timer_shorts_enable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
    }
    else
    {
        // FEM didn't schedule anything on timer, so the timer may be stopped when radio ramp-up
        // is triggered
        nrf_timer_shorts_enable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE1_STOP_MASK);
    }

    nrf_radio_event_clear(NRF_RADIO_EVENT_PHYEND);
#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
    nrf_radio_event_clear(NRF_RADIO_EVENT_ADDRESS);
#endif

    nrf_ppi_channel_enable(PPI_TIMER_TX_ACK);

    // Since this point the transmission is armed on TIMER's CC1

    // Detect if PPI will work in future or has just worked.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_CAPTURE3);
    if (nrf_timer_cc_read(NRF_802154_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL3) <
        nrf_timer_cc_read(NRF_802154_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL1))
    {
        result = true;
    }
    else
    {
        ppi_and_egu_delay_wait();

        if (nrf_radio_state_get() == NRF_RADIO_STATE_TXRU)
        {
            result = true;
        }
        else if (nrf_radio_event_check(NRF_RADIO_EVENT_TXREADY))
        {
            result = true;
        }
        else
        {
            result = false;
        }
    }

    if (result)
    {
        uint32_t ints_to_enable = NRF_RADIO_INT_PHYEND_MASK;

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
        ints_to_enable |= NRF_RADIO_INT_ADDRESS_MASK;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

        nrf_radio_int_enable(ints_to_enable);
    }
    else
    {
        /* We were to late with setting up PPI_TIMER_ACK, ack transmission was not triggered and
         * will not be triggered in future.
         */
        nrf_ppi_channel_disable(PPI_TIMER_TX_ACK);

        /* As the timer was running during operation, it is possible we were able to configure
         * FEM thus it may trigger in future or may started PA activation.
         */
        nrf_802154_fal_pa_configuration_clear(&m_activate_tx_cc0_timeshifted, NULL);

        nrf_802154_fal_deactivate_now(NRF_802154_FAL_PA);

        nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

        /* No callbacks will be called */
    }

    return result;
}

static inline void wait_until_radio_is_disabled(void)
{
    while (nrf_radio_state_get() != NRF_RADIO_STATE_DISABLED)
    {
        /* Intentionally empty */
    }
}

static void rxframe_finish_disable_ppis(void)
{
    /* Disable PPIs used by receive operation */
    nrf_ppi_channel_disable(PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(PPI_EGU_RAMP_UP);
#if !NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_fork_endpoint_setup(PPI_EGU_RAMP_UP, 0);
#endif // NRF_802154_DISABLE_BCC_MATCHING

#if !NRF_802154_DISABLE_BCC_MATCHING && defined(NRF_RADIO_EVENT_HELPER1)
    nrf_ppi_channel_disable(PPI_RADIO_HELPER1_EGU_HELPER1);
#endif

    nrf_ppi_channel_disable(PPI_EGU_TIMER_START);
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_fork_endpoint_setup(PPI_EGU_TIMER_START, 0);
#endif // NRF_802154_DISABLE_BCC_MATCHING

#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_disable(PPI_CRCERROR_CLEAR);
    nrf_ppi_channel_disable(PPI_CRCOK_DIS_PPI);
    nrf_ppi_channel_disable(PPI_ADDRESS_COUNTER_COUNT);
    nrf_ppi_channel_disable(PPI_CRCERROR_COUNTER_CLEAR);
#endif // NRF_802154_DISABLE_BCC_MATCHING

    nrf_ppi_channel_remove_from_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);
}

static void rxframe_finish_disable_fem_activation(void)
{
    // Disable LNA activation
    nrf_802154_fal_lna_configuration_clear(&m_activate_rx_cc0, NULL);
    // Disable short used by LNA activation
    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE,
                             NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
}

static void rxframe_finish_disable_ints(void)
{
    uint32_t ints_to_disable = NRF_RADIO_INT_CRCOK_MASK | NRF_RADIO_INT_ADDRESS_MASK;

#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
    ints_to_disable |= NRF_RADIO_INT_CRCERROR_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
#if !NRF_802154_DISABLE_BCC_MATCHING
    ints_to_disable |= NRF_RADIO_INT_BCMATCH_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_int_disable(ints_to_disable);

#if !NRF_802154_DISABLE_BCC_MATCHING && defined(NRF_RADIO_EVENT_HELPER1)
    nrf_egu_int_disable(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_INTMASK);
#endif
}

static void rxframe_finish_psdu_is_not_being_received(void)
{
#if NRF_802154_DISABLE_BCC_MATCHING
    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_timer_shorts_disable(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE1_STOP_MASK);
#else
    m_flags.psdu_being_received = false;
#endif // NRF_802154_DISABLE_BCC_MATCHING
}

static void rxframe_finish(void)
{
    /* Note that CRCOK/CRCERROR event is generated several cycles before END event.
     *
     * Below shown what is happening in the hardware
     *
     * TIMER is started by path:
     * RADIO.SHORT_END_DISABLE -> RADIO.TASKS_DISABLE -> RADIO.EVENTS_DISABLED ->
     *    PPI_DISABLED_EGU -> EGU.TASKS_TRIGGER -> EGU.EVENTS_TRIGGERED -> PPI_EGU_TIMER_START -> TIMER.TASKS_START
     *
     * FEM's LNA mode is disabled by path:
     * RADIO.SHORT_END_DISABLE -> RADIO.TASKS_DISABLE -> RADIO.EVENTS_DISABLED -> (FEM's PPI triggering disable LNA operation,
     *                                                                             see m_deactivate_on_disable)
     *
     * RADIO will not ramp up, as PPI_EGU_RAMP_UP channel is self-disabling, and
     * it was disabled when receive ramp-up was started.
     */
    wait_until_radio_is_disabled(); // This includes waiting since CRCOK/CRCERROR (several cycles) event until END
                                    // and then during RXDISABLE state (0.5us)

    ppi_and_egu_delay_wait();

    /* Now it is guaranteed, that:
     * - FEM operation to disable LNA mode is triggered through FEM's PPIs
     * - TIMER is started again allowing operation of nrf_802154_trx_transmit_ack
     */

    rxframe_finish_disable_ppis();
    rxframe_finish_disable_fem_activation();
    rxframe_finish_psdu_is_not_being_received();
    rxframe_finish_disable_ints();
    nrf_radio_shorts_set(SHORTS_IDLE);
    m_flags.missing_receive_buffer = false;

    /* Current state of peripherals:
     * RADIO is DISABLED
     * FEM is powered but LNA mode has just been turned off
     * TIMER is running, counting from the value stored in m_timer_value_on_radio_end_event
     * All PPIs used by receive operation are disabled, forks are cleared, PPI groups that were used are cleared
     * RADIO.SHORTS are cleared
     */
}

void nrf_802154_trx_abort(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_DISABLED:
        case TRX_STATE_IDLE:
        case TRX_STATE_FINISHED:
            /* Nothing to do, intentionally empty */
            break;

        case TRX_STATE_GOING_IDLE:
            go_idle_abort();
            break;

        case TRX_STATE_RXFRAME:
            receive_frame_abort();
            break;

        case TRX_STATE_RXFRAME_FINISHED:
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
            m_trx_state = TRX_STATE_FINISHED;
            break;

        case TRX_STATE_RXACK:
            receive_ack_abort();
            break;

        case TRX_STATE_TXFRAME:
            transmit_frame_abort();
            break;

        case TRX_STATE_TXACK:
            transmit_ack_abort();
            break;

        case TRX_STATE_STANDALONE_CCA:
            standalone_cca_abort();
            break;

        case TRX_STATE_CONTINUOUS_CARRIER:
            continuous_carrier_abort();
            break;

        case TRX_STATE_ENERGY_DETECTION:
            energy_detection_abort();
            break;

        default:
            assert(false);
    }
}

static void go_idle_from_state_finished(void)
{
    m_trx_state = TRX_STATE_GOING_IDLE;

    nrf_radio_event_clear(NRF_RADIO_EVENT_DISABLED);
    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);

    nrf_radio_int_enable(NRF_RADIO_INT_DISABLED_MASK);
}

bool nrf_802154_trx_go_idle(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_DISABLED:
            assert(false);
            break;

        case TRX_STATE_IDLE:
            /* There will be no callout */
            return false;

        case TRX_STATE_GOING_IDLE:
            /* There will be callout */
            return true;

        case TRX_STATE_RXFRAME_FINISHED:
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
        /* fallthrough */

        case TRX_STATE_FINISHED:
            go_idle_from_state_finished();
            return true;

        default:
            assert(false);
    }

    return false;
}

static void go_idle_abort(void)
{
    nrf_radio_int_disable(NRF_RADIO_INT_DISABLED_MASK);
    m_trx_state = TRX_STATE_FINISHED;
}

static void receive_frame_abort(void)
{
    rxframe_finish_disable_ppis();
    rxframe_finish_disable_fem_activation();
    rxframe_finish_psdu_is_not_being_received();
    rxframe_finish_disable_ints();
    nrf_radio_shorts_set(SHORTS_IDLE);

    m_flags.missing_receive_buffer = false;
    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);

    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

    m_trx_state = TRX_STATE_FINISHED;
}

static void rxack_finish_disable_ppis(void)
{
    nrf_ppi_channel_disable(PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(PPI_EGU_RAMP_UP);
    nrf_ppi_fork_endpoint_setup(PPI_EGU_RAMP_UP, 0);
    nrf_ppi_channel_disable(PPI_EGU_TIMER_START);
    nrf_ppi_channel_remove_from_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);
}

static void rxack_finish_disable_ints(void)
{
    nrf_radio_int_disable(
        NRF_RADIO_INT_ADDRESS_MASK | NRF_RADIO_INT_CRCERROR_MASK | NRF_RADIO_INT_CRCOK_MASK);
}

static void rxack_finish_disable_fem_activation(void)
{
    // Disable LNA activation
    nrf_802154_fal_lna_configuration_clear(&m_activate_rx_cc0, NULL);

    // Disable short used by LNA activation
    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE,
                             NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
}

static void rxack_finish(void)
{
    rxack_finish_disable_ppis();
    rxack_finish_disable_ints();
    rxack_finish_disable_fem_activation();
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_radio_shorts_set(SHORTS_IDLE);
    m_flags.missing_receive_buffer = false;

    /* Current state of peripherals
     * RADIO is DISABLED
     * FEM is powered but LNA mode has just been turned off
     * TIMER is shutdown
     * PPIs used by receive operation are disabled, forks are cleared, PPI groups used are cleared
     * RADIO.SHORTS are cleared
     */
}

static void receive_ack_abort(void)
{
    rxack_finish_disable_ppis();
    rxack_finish_disable_ints();
    rxack_finish_disable_fem_activation();
    nrf_radio_shorts_set(SHORTS_IDLE);
    m_flags.missing_receive_buffer = false;

    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);

    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

    m_trx_state = TRX_STATE_FINISHED;
}

void nrf_802154_trx_standalone_cca(void)
{
    assert((m_trx_state == TRX_STATE_IDLE) || (m_trx_state == TRX_STATE_FINISHED));

    m_trx_state = TRX_STATE_STANDALONE_CCA;

    // Set shorts
    nrf_radio_shorts_set(SHORTS_CCA);

    // Enable IRQs
    nrf_radio_event_clear(NRF_RADIO_EVENT_CCABUSY);
    nrf_radio_event_clear(NRF_RADIO_EVENT_CCAIDLE);
    nrf_radio_int_enable(NRF_RADIO_INT_CCABUSY_MASK | NRF_RADIO_INT_CCAIDLE_MASK);

    // Set FEM
    fem_for_lna_set();

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
    ppis_for_egu_and_ramp_up_set(NRF_RADIO_TASK_RXEN, true);

    trigger_disable_to_start_rampup();
}

static void standalone_cca_finish(void)
{
    nrf_ppi_channel_disable(PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(PPI_EGU_RAMP_UP);
    nrf_ppi_channel_remove_from_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);

    nrf_radio_shorts_set(SHORTS_IDLE);

    fem_for_lna_reset();

    nrf_radio_int_disable(NRF_RADIO_INT_CCABUSY_MASK | NRF_RADIO_INT_CCAIDLE_MASK);

    nrf_radio_task_trigger(NRF_RADIO_TASK_CCASTOP);
    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);
}

static void standalone_cca_abort(void)
{
    standalone_cca_finish();

    m_trx_state = TRX_STATE_FINISHED;
}

void nrf_802154_trx_continuous_carrier(void)
{
    assert((m_trx_state == TRX_STATE_IDLE) || (m_trx_state == TRX_STATE_FINISHED));

    m_trx_state = TRX_STATE_CONTINUOUS_CARRIER;

    // Set Tx Power
    nrf_radio_txpower_set(nrf_802154_pib_tx_power_get());

    // Set FEM
    fem_for_pa_set();

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
    ppis_for_egu_and_ramp_up_set(NRF_RADIO_TASK_TXEN, false);

    trigger_disable_to_start_rampup();
}

void nrf_802154_trx_continuous_carrier_restart(void)
{
    assert(m_trx_state == TRX_STATE_CONTINUOUS_CARRIER);

    // Continuous carrier PPIs are configured without self-disabling
    // Triggering RADIO.TASK_DISABLE causes ramp-down -> RADIO.EVENTS_DISABLED -> EGU.TASK -> EGU.EVENT ->
    // RADIO.TASK_TXEN -> ramp_up -> new continous carrier

    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);
}

static void continuous_carrier_abort(void)
{
    nrf_ppi_channel_disable(PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(PPI_EGU_RAMP_UP);

    fem_for_pa_reset();

    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);

    m_trx_state = TRX_STATE_FINISHED;
}

void nrf_802154_trx_energy_detection(uint32_t ed_count)
{
    assert((m_trx_state == TRX_STATE_FINISHED) || (m_trx_state == TRX_STATE_IDLE));

    m_trx_state = TRX_STATE_ENERGY_DETECTION;

    ed_count--;
    /* Check that vd_count will fit into defined bits of register */
    assert( (ed_count & (~RADIO_EDCNT_EDCNT_Msk)) == 0U);

    nrf_radio_ed_loop_count_set(ed_count);

    // Set shorts
    nrf_radio_shorts_set(SHORTS_ED);

    // Enable IRQs
    nrf_radio_event_clear(NRF_RADIO_EVENT_EDEND);
    nrf_radio_int_enable(NRF_RADIO_INT_EDEND_MASK);

    // Set FEM
    fem_for_lna_set();

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
    ppis_for_egu_and_ramp_up_set(NRF_RADIO_TASK_RXEN, true);

    trigger_disable_to_start_rampup();
}

static void energy_detection_finish(void)
{
    nrf_ppi_channel_disable(PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(PPI_EGU_RAMP_UP);
    nrf_ppi_fork_endpoint_setup(PPI_EGU_RAMP_UP, 0);
    nrf_ppi_channel_remove_from_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);

    fem_for_lna_reset();

    nrf_radio_int_disable(NRF_RADIO_INT_EDEND_MASK);
    nrf_radio_shorts_set(SHORTS_IDLE);

    nrf_radio_task_trigger(NRF_RADIO_TASK_EDSTOP);
    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);
}

static void energy_detection_abort(void)
{
    energy_detection_finish();
    m_trx_state = TRX_STATE_FINISHED;
}

static void irq_handler_address(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_RXFRAME:
            nrf_802154_trx_receive_frame_started();
            break;

        case TRX_STATE_RXACK:
            m_flags.rssi_started = true;
            nrf_802154_trx_receive_ack_started();
            break;

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
        case TRX_STATE_TXFRAME:
            nrf_radio_int_disable(NRF_RADIO_INT_ADDRESS_MASK);
            m_flags.tx_started = true;
            nrf_802154_trx_transmit_frame_started();
            break;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
        case TRX_STATE_TXACK:
            nrf_radio_int_disable(NRF_RADIO_INT_ADDRESS_MASK);
            nrf_802154_trx_transmit_ack_started();
            break;
#endif

        default:
            assert(false);
    }
}

#if !NRF_802154_DISABLE_BCC_MATCHING
static void irq_handler_bcmatch(void)
{
    uint8_t current_bcc;
    uint8_t next_bcc;

    assert(m_trx_state == TRX_STATE_RXFRAME);

    m_flags.psdu_being_received = true;

    // If CRCERROR event is set, it means that events are handled out of order due to software
    // latency. Just skip this handler in this case - frame will be dropped.
    if (nrf_radio_event_check(NRF_RADIO_EVENT_CRCERROR))
    {
        return;
    }

    current_bcc = nrf_radio_bcc_get() / 8U;

    next_bcc = nrf_802154_trx_receive_frame_bcmatched(current_bcc);

    if (next_bcc > current_bcc)
    {
        /* Note: If we don't make it before given octet is received by RADIO bcmatch will not be triggered.
         * The fact that filtering may be not completed at the call to nrf_802154_trx_receive_received handler
         * should be handled by the handler.
         */
        nrf_radio_bcc_set(next_bcc * 8);
    }
}

#endif

#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
static void irq_handler_crcerror(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_RXFRAME:
#if NRF_802154_DISABLE_BCC_MATCHING
            /* The hardware restarts receive at the moment.
             * The TIMER is being shutdown and restarted by the hardware see
             * PPI_CRCERROR_CLEAR in nrf_802154_trx_receive_frame
             */
#else
            rxframe_finish();
            /* On crc error TIMER is not needed, no ACK may be sent */
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
            m_trx_state = TRX_STATE_FINISHED;
#endif
            nrf_802154_trx_receive_frame_crcerror();
            break;

        case TRX_STATE_RXACK:
            rxack_finish();
            m_trx_state = TRX_STATE_FINISHED;
            nrf_802154_trx_receive_ack_crcerror();
            break;

        default:
            assert(false);
    }
}

#endif

static void irq_handler_crcok(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_RXFRAME:
            m_flags.rssi_started = true;
            rxframe_finish();
            m_trx_state = TRX_STATE_RXFRAME_FINISHED;
            nrf_802154_trx_receive_frame_received();
            break;

        case TRX_STATE_RXACK:
            rxack_finish();
            m_trx_state = TRX_STATE_FINISHED;
            nrf_802154_trx_receive_ack_received();
            break;

        default:
            assert(false);
    }
}

static void txframe_finish_disable_ppis(void)
{
    nrf_ppi_channel_disable(PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(PPI_EGU_RAMP_UP);
    nrf_ppi_fork_endpoint_setup(PPI_EGU_RAMP_UP, 0);
    nrf_ppi_channel_remove_from_group(PPI_EGU_RAMP_UP, PPI_CHGRP0);
}

static void txframe_finish_disable_ints(void)
{
    nrf_radio_int_disable(
        NRF_RADIO_INT_PHYEND_MASK | NRF_RADIO_INT_CCABUSY_MASK | NRF_RADIO_INT_ADDRESS_MASK);
}

static void txframe_finish(void)
{
    /* Below shown what is happening in the hardware
     *
     * Due to short RADIO.SHORT_PHYEND_DISABLE the RADIO is in TXDISABLE (currently ramping down for 21us) state or
     * has already reached DISABLED state (if we entered into ISR with greater latency)
     *
     * Even if RADIO reaches state DISABLED (event DISABLED was triggered by short), no transmission will be triggered
     * as PPI_EGU_RAMP_UP is self-disabling PPI channel.
     *
     * If FEM is in use the PPI_EGU_TIMER_START might be triggered if radio reached DISABLED state,
     * so the TIMER may start counting from the value on which FEM activation finished. The TIMER's CC registers
     * are set in the past so even if TIMER started no spurious FEM PA activation will occur.
     * We need to disable PPI_EGU_TIMER_START and then shutdown TIMER as it is not used.
     */
    txframe_finish_disable_ppis();

    fem_for_tx_reset(m_transmit_with_cca, true);

    txframe_finish_disable_ints();

    nrf_radio_shorts_set(SHORTS_IDLE);

    m_flags.tx_started             = false;
    m_flags.missing_receive_buffer = false;

    /* Current state of peripherals
     * RADIO is either in TXDISABLE or DISABLED
     * FEM is powered but PA mode will be turned off on entry into DISABLED state or is already turned off
     * TIMER is shutdown
     * All PPIs that were used are disabled (forks are cleared if used)
     * RADIO.SHORTS are cleared
     */
}

static void transmit_frame_abort(void)
{
    txframe_finish_disable_ppis();
    nrf_radio_shorts_set(SHORTS_IDLE);

    fem_for_tx_reset(m_transmit_with_cca, true);

    txframe_finish_disable_ints();

    m_flags.tx_started             = false;
    m_flags.missing_receive_buffer = false;

    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);

    m_trx_state = TRX_STATE_FINISHED;
}

static void txack_finish(void)
{
    /* Below shown what is happening in the hardware
     *
     * Due to short RADIO.SHORT_PHYEND_DISABLE the RADIO is in TXDISABLE (currently ramping down for 21us) state or
     * has already reached DISABLED state (if we entered into ISR with greater latency)
     *
     * Even if RADIO reaches state DISABLED (event DISABLED was triggered by short), no transmission will be triggered
     * as only PPI_TIMER_TX_ACK was enabled.
     *
     * FEM will disable PA mode on RADIO.DISABLED event
     *
     * The TIMER was counting to trigger RADIO ramp up and FEM (if required). The TIMER is configured
     * to trigger STOP task on one of these events (whichever is later). As we finished the TIMER is
     * stopped now, and there is no PPIs starting it automatically by the hardware.
     */
    nrf_ppi_channel_disable(PPI_TIMER_TX_ACK);

    nrf_radio_shorts_set(SHORTS_IDLE);

    nrf_802154_fal_pa_configuration_clear(&m_activate_tx_cc0_timeshifted, NULL);

    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE,
                             NRF_TIMER_SHORT_COMPARE0_STOP_MASK |
                             NRF_TIMER_SHORT_COMPARE1_STOP_MASK);

    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

    nrf_radio_int_disable(NRF_RADIO_INT_PHYEND_MASK | NRF_RADIO_INT_ADDRESS_MASK);

    /* Current state of peripherals
     * RADIO is either in TXDISABLE or DISABLED
     * FEM is powered but PA mode will be turned off on entry into DISABLED state or is already turned off
     * TIMER is shutdown
     * All PPIs that were used are disabled (forks are cleared if used)
     * RADIO.SHORTS are cleared
     */
}

static void transmit_ack_abort(void)
{
    nrf_ppi_channel_disable(PPI_TIMER_TX_ACK);

    nrf_radio_shorts_set(SHORTS_IDLE);

    nrf_802154_fal_pa_configuration_clear(&m_activate_tx_cc0_timeshifted, NULL);

    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE,
                             NRF_TIMER_SHORT_COMPARE0_STOP_MASK |
                             NRF_TIMER_SHORT_COMPARE1_STOP_MASK);

    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

    nrf_radio_int_disable(NRF_RADIO_INT_PHYEND_MASK | NRF_RADIO_INT_ADDRESS_MASK);

    nrf_radio_task_trigger(NRF_RADIO_TASK_DISABLE);

    m_trx_state = TRX_STATE_FINISHED;
}

static void irq_handler_phyend(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_TXFRAME:
            txframe_finish();
            m_trx_state = TRX_STATE_FINISHED;
            nrf_802154_trx_transmit_frame_transmitted();
            break;

        case TRX_STATE_TXACK:
            txack_finish();
            m_trx_state = TRX_STATE_FINISHED;
            nrf_802154_trx_transmit_ack_transmitted();
            break;

        default:
            assert(false);
    }
}

static void go_idle_finish(void)
{
    nrf_radio_int_disable(NRF_RADIO_INT_DISABLED_MASK);

    fem_power_down_now();

    m_trx_state = TRX_STATE_IDLE;

    nrf_802154_trx_go_idle_finished();
}

static void irq_handler_disabled(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_GOING_IDLE:
            go_idle_finish();
            break;

        default:
            assert(false);
    }
}

static void irq_handler_ccaidle(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_STANDALONE_CCA:
            standalone_cca_finish();
            m_trx_state = TRX_STATE_FINISHED;
            nrf_802154_trx_standalone_cca_finished(true);
            break;

        default:
            assert(false);
    }
}

static void irq_handler_ccabusy(void)
{
    switch (m_trx_state)
    {
        case TRX_STATE_TXFRAME:
            assert(m_transmit_with_cca);
            txframe_finish();
            m_trx_state = TRX_STATE_FINISHED;
            nrf_802154_trx_transmit_frame_ccabusy();
            break;

        case TRX_STATE_STANDALONE_CCA:
            standalone_cca_finish();
            m_trx_state = TRX_STATE_FINISHED;
            nrf_802154_trx_standalone_cca_finished(false);
            break;

        default:
            assert(false);
    }
}

static void irq_handler_edend(void)
{
    assert(m_trx_state == TRX_STATE_ENERGY_DETECTION);

    uint8_t ed_sample = nrf_radio_ed_sample_get();

    energy_detection_finish();
    m_trx_state = TRX_STATE_FINISHED;

    nrf_802154_trx_energy_detection_finished(ed_sample);
}

#if defined(NRF_RADIO_EVENT_HELPER1)
static void irq_handler_helper1(void)
{
    assert(m_trx_state == TRX_STATE_RXFRAME);

    nrf_802154_trx_receive_frame_prestarted();
}

#endif

void nrf_802154_radio_irq_handler(void)
{
    nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_IRQ_HANDLER);

    // Prevent interrupting of this handler by requests from higher priority code.
    bool result = nrf_802154_critical_section_enter();

    assert(result);
    (void)result;

#if defined(NRF_RADIO_EVENT_HELPER1)
    // Note: For NRF_RADIO_EVENT_HELPER1 we enable interrupt through EGU.
    // That's why we check here EGU's EGU_HELPER1_INTMASK.
    // The RADIO does not have interrupt from HELPER1 event.
    if (nrf_egu_int_enable_check(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_INTMASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_HELPER1))
    {
        nrf_radio_event_clear(NRF_RADIO_EVENT_HELPER1);
        nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_EVENT);

        irq_handler_helper1();
    }
#endif

    if (nrf_radio_int_enable_check(NRF_RADIO_INT_ADDRESS_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_ADDRESS))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_FRAMESTART);
        nrf_radio_event_clear(NRF_RADIO_EVENT_ADDRESS);

        irq_handler_address();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_FRAMESTART);
    }

#if !NRF_802154_DISABLE_BCC_MATCHING
    // Check MAC frame header.
    if (nrf_radio_int_enable_check(NRF_RADIO_INT_BCMATCH_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_BCMATCH))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_BCMATCH);
        nrf_radio_event_clear(NRF_RADIO_EVENT_BCMATCH);

        irq_handler_bcmatch();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_BCMATCH);
    }

#endif // !NRF_802154_DISABLE_BCC_MATCHING

#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
    if (nrf_radio_int_enable_check(NRF_RADIO_INT_CRCERROR_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_CRCERROR))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_CRCERROR);
        nrf_radio_event_clear(NRF_RADIO_EVENT_CRCERROR);

        irq_handler_crcerror();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_CRCERROR);
    }
#endif // !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR

    if (nrf_radio_int_enable_check(NRF_RADIO_INT_CRCOK_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_CRCOK))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_CRCOK);
        nrf_radio_event_clear(NRF_RADIO_EVENT_CRCOK);

        irq_handler_crcok();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_CRCOK);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO_INT_PHYEND_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_PHYEND))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_PHYEND);
        nrf_radio_event_clear(NRF_RADIO_EVENT_PHYEND);

        irq_handler_phyend();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_PHYEND);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO_INT_DISABLED_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_DISABLED))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_DISABLED);
        nrf_radio_event_clear(NRF_RADIO_EVENT_DISABLED);

        irq_handler_disabled();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_DISABLED);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO_INT_CCAIDLE_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_CCAIDLE))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_CCAIDLE);
        nrf_radio_event_clear(NRF_RADIO_EVENT_CCAIDLE);

        irq_handler_ccaidle();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_CCAIDLE);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO_INT_CCABUSY_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_CCABUSY))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_CCABUSY);
        nrf_radio_event_clear(NRF_RADIO_EVENT_CCABUSY);

        irq_handler_ccabusy();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_CCABUSY);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO_INT_EDEND_MASK) &&
        nrf_radio_event_check(NRF_RADIO_EVENT_EDEND))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_EDEND);
        nrf_radio_event_clear(NRF_RADIO_EVENT_EDEND);

        irq_handler_edend();

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_EDEND);
    }

    nrf_802154_critical_section_exit();

    nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_IRQ_HANDLER);
}

#if NRF_802154_INTERNAL_RADIO_IRQ_HANDLING
void RADIO_IRQHandler(void)
{
    nrf_802154_radio_irq_handler();
}

#endif // NRF_802154_INTERNAL_RADIO_IRQ_HANDLING

#if defined(NRF_RADIO_EVENT_HELPER1)
void nrf_802154_trx_swi_irq_handler(void)
{
    if (nrf_egu_int_enable_check(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_INTMASK) &&
        nrf_egu_event_check(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_EVENT))
    {
        nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_HELPER1_EVENT);

        // We are in SWI_IRQHandler, which priority is usually lower than RADIO_IRQHandler.
        // To avoid problems with critical sections, trigger RADIO_IRQ manually.
        // - If we are not in critical section, RADIO_IRQ will start shortly (calling
        // nrf_802154_radio_irq_handler) preempting current SWI_IRQHandler. From
        // nrf_802154_radio_irq_handler we acquire critical section and
        // process helper1 event.
        // If we are in critical section, the RADIO_IRQ is disabled on NVIC.
        // Following will make it pending, and processing of RADIO_IRQ will start
        // when critical section is left.

        NVIC_SetPendingIRQ(RADIO_IRQn);
    }
}

#endif
