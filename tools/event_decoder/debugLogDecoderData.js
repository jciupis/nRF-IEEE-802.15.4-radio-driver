// This file provides auxliary data required by debug log
// Stuff here is related with C source code.
// If you add new modules, module's local events or global events 
// please update this file.


// Description of modules that may be present in logs
// Each member of the array is an object with following fields:
// id (int) - Module id, set up in C source code by NRF_802154_DEBUG_LOG_MODULE_ID
// name (string) - Name of the module to be displayed on sequence diagram
// localEvents (array of objects or undefined) - local events that may be logged by the module. This is array of objects with following fields:
//    id (int) - identifier of an event (See C defines NRF_802154_DEBUG_LOG_LOCAL_EVENT_ID_<MODULE_NAME>_xxxx
//    text (string) - human readable text describing an event (will be visible in sequence diagram)
//    paramType (string or undefined) - selects type of param
//        undefined - event has no parameter, parameter won't be displayed
//        "uint" - prints parameter as unsigned integer
//        "enum" - prints parameter as enum according to enumValues field
//    enumValues (array of objects) - if paramType == "enum" it is an array of objects with following fields:
//        value (int) - numerical value of enum used in C code
//        text (string) - text to be displayed as enum value
var debugLogDecoderModules = [
    {
        id: 1,
        name: "APPLICATION",        
    },
    {
        id: 2,
        name: "CORE",
        localEvents: [
            {id: 1, text: "SET_STATE", paramType: "enum", enumValues: [
                // See nrf_802154_core.c enum radio_state_t
                {value: 0, text: "SLEEP"},
                {value: 1, text: "FALLING_ASLEEP"},
                {value: 2, text: "RX"},
                {value: 3, text: "TX_ACK"},
                {value: 4, text: "CCA_TX"},
                {value: 5, text: "TX"},
                {value: 6, text: "RX_ACK"},
                {value: 7, text: "ED"},
                {value: 8, text: "CCA"},
                {value: 9, text: "CONTINUOUS_CARRIER"},
                {value: 10, text: "MODULATED_CARRIER"}
            ]}
        ]
    },
    {
        id: 3,
        name: "RSCH",
        localEvents: [
            {id: 1, text: "PRIORITY_SET", paramType: "enum", enumValues: [
                // See nrf_802154_rsch.h enum rsch_prio_t
                {value: 0, text: "IDLE"},
                {value: 1, text: "IDLE_LISTENING"},
                {value: 2, text: "RX"},
                {value: 3, text: "DETECT"},
                {value: 4, text: "TX"}
            ]}
        ]
    },
    {
        id: 4,
        name: "CRITICAL_SECTION"        
    },
    {
        id: 5,
        name: "TIMER_COORD"
    },
    {
        id: 6,
        name: "TRX"
    },
    {
        id: 7,
        name: "TIMER_SCHED",        
    },
    {
        id: 8,
        name: "CSMACA"
    },
    {
        id: 9,
        name: "DELAYED_TRX"        
    },
    {
        id: 10,
        name: "ACK_TIMEOUT"
    },
    {
        id: 11,
        name: "RAAL",
        localEvents: [
            {id: 1, text: "TIMESLOT_REQUEST", paramType: "uint"},
            {id: 2, text: "TIMESLOT_REQUEST_RESULT", paramType: "uint"},
        ]
    }
];

// Map for decoding logs generated in C code with nrf_802154_debug_log_global_event
// This is array of objects having following fields:
//    id (int) - identifier of an event (See C defines NRF_802154_DEBUG_LOG_GLOBAL_EVENT_ID_xxxx
//    text (string) - humen readable text describing an event (will be visible in sequence diagram)
//    paramType (string or undefined) - selects type of param
//        undefined - event has no parameter, parameter won't be displayed
//        "uint" - prints parameter as unsigned integer
//        "enum" - prints parameter as enum according to enumValues field
//    enumValues (array of objects) - if paramType == "enum" it is an array of objects with following fields:
//        value (int) - numerical value of enum used in C code
//        text (string) - text to be displayed as enum value

var debugLogDecoderGlobalEvents = [
    { id: 1, text: "RADIO_RESET" },
    { id: 2, text: "TIMESTAMP_RAW", paramType: "uint" }
];

