#pragma once

#include "air/air_cmd.h"
#include "air/air_io.h"
#include "air/air_freq.h"
#include "air/air_lora.h"
#include "air/air_stream.h"

#include "output/output.h"

#include "rmp/rmp_air.h"

#include "util/time.h"

#include "msp/msp_air.h"

typedef struct lora_s lora_t;

typedef struct output_air_config_s
{
    int tx_power; // dbm
} output_air_config_t;

typedef struct output_air_s
{
    output_t output;
    air_io_t air;
    air_lora_mode_e air_mode;
    air_lora_mode_e air_mode_fastest;
    air_lora_mode_e air_mode_longest;
    air_lora_mode_e requested_air_mode;
    time_micros_t start_switch_air_mode_up_at;
    time_micros_t start_switch_air_mode_down_at;
    air_cmd_switch_mode_ack_t switch_air_mode;
    bool is_listening;
    bool force_stream_feed;
    lora_t *lora;
    air_lora_band_e band;
    time_micros_t last_downlink_packet_at;
    time_micros_t full_cycle_time;
    time_micros_t uplink_cycle_time;
    time_micros_t next_packet;
    bool rx_done;
    unsigned seq : AIR_SEQ_BITS;
    unsigned freq_index;
    air_freq_table_t freq_table;
    air_stream_t air_stream;
    bool expecting_downlink_packet;
    unsigned consecutive_downlink_lost_packets;
    int tx_power;

    msp_air_t msp_air;
    rmp_air_t rmp_air;
} output_air_t;

void output_air_init(output_air_t *output, air_addr_t addr, lora_t *lora, air_lora_band_e band, rmp_t *rmp);

void output_air_set_tx_power(output_air_t *output, int tx_power);