#include <assert.h>
#include <string.h>

#include <hal/log.h>

#include "config/settings.h"

#include "msp/msp.h"

#include "rc/telemetry.h"

#include "util/macros.h"

#include "output.h"

static const char *TAG = "Output";

#define OUTPUT_FC_MSP_UPDATE_INTERVAL SECS_TO_MICROS(10)
#define MSP_SEND_REQ(output, req) OUTPUT_MSP_SEND_REQ(output, req, output_msp_callback)
#define FW_VARIANT_CONST(s0, s1, s2, s3) (s0 << 24 | s1 << 16 | s2 << 8 | s3)
#define FW_VARIANT_CONST_S(s) FW_VARIANT_CONST(s[0], s[1], s[2], s[3])

static void telemetry_updated_callback(void *data, telemetry_downlink_id_e id, telemetry_val_t *val)
{
    output_t *output = data;
    telemetry_t *telemetry = &output->rc_data->telemetry_downlink[TELEMETRY_DOWNLINK_GET_IDX(id)];
    // TODO: Pass now to the callback
    time_micros_t now = time_micros_now();
    bool changed = false;
    if (!telemetry_value_is_equal(telemetry, id, val))
    {
        changed = true;
        // TODO: Optimize this copy, we don't need to copy the whole value
        // for small types
        telemetry->val = *val;
    }
    data_state_update(&telemetry->data_state, changed, now);
}

static void telemetry_calculate_callback(void *data, telemetry_downlink_id_e id)
{
    output_t *output = data;
    switch (id)
    {
    case TELEMETRY_ID_BAT_REMAINING_P:
    {
        TELEMETRY_ASSERT_TYPE(TELEMETRY_ID_BAT_CAPACITY, TELEMETRY_TYPE_UINT16);
        TELEMETRY_ASSERT_TYPE(TELEMETRY_ID_CURRENT_DRAWN, TELEMETRY_TYPE_INT32);
        telemetry_t *capacity = rc_data_get_telemetry(output->rc_data, TELEMETRY_ID_BAT_CAPACITY);
        telemetry_t *drawn = rc_data_get_telemetry(output->rc_data, TELEMETRY_ID_CURRENT_DRAWN);
        if (capacity->val.u16 > 0 && drawn->val.i32 >= 0)
        {
            int32_t rem = capacity->val.u16 - drawn->val.i32;
            if (rem < 0)
            {
                rem = 0;
            }
            OUTPUT_TELEMETRY_UPDATE_U8(output, TELEMETRY_ID_BAT_REMAINING_P, (100 * rem) / capacity->val.u16);
        }
        break;
    }
    default:
        break;
    }
}

static void output_setting_changed(const setting_t *setting, void *user_data)
{
    output_t *output = user_data;
    if (SETTING_IS(setting, SETTING_KEY_RX_AUTO_CRAFT_NAME))
    {
        // Re-schedule polls to take into account if the setting
        // is enabled or disabled.
        output->fc.next_fw_update = 0;
    }
}

static void output_msp_configure_poll(output_t *output, uint16_t cmd, time_micros_t interval)
{
    for (int ii = 0; ii < OUTPUT_FC_MAX_NUM_POLLS; ii++)
    {
        if (output->fc.polls[ii].interval == 0)
        {
            // We found a free one
            output->fc.polls[ii].cmd = cmd;
            output->fc.polls[ii].interval = interval;
            output->fc.polls[ii].next_poll = 0;
            return;
        }
    }
    // Couldn't find a free slot, we need to raise OUTPUT_FC_MAX_NUM_POLLS
    assert(0 && "Couldn't add MSP poll, raise OUTPUT_FC_MAX_NUM_POLLS");
}

static void output_msp_configure_polling_common(output_t *output)
{
    if (settings_get_key_bool(SETTING_KEY_RX_AUTO_CRAFT_NAME))
    {
        output->craft_name_setting = settings_get_key(SETTING_KEY_RX_CRAFT_NAME);
        output_msp_configure_poll(output, MSP_NAME, SECS_TO_MICROS(10));
    }
    else
    {
        output->craft_name_setting = NULL;
    }
    if (!OUTPUT_HAS_FLAG(output, OUTPUT_FLAG_SENDS_RSSI))
    {
        output_msp_configure_poll(output, MSP_RSSI_CONFIG, SECS_TO_MICROS(10));
    }
}

static void output_msp_configure_polling_inav(output_t *output)
{
}

static void output_msp_configure_polling_betaflight(output_t *output)
{
}

static void output_msp_configure_polling(output_t *output)
{
    memset(output->fc.polls, 0, sizeof(output->fc.polls));
    output_msp_configure_polling_common(output);
    switch (FW_VARIANT_CONST_S(output->fc.fw_variant))
    {
    case FW_VARIANT_CONST('I', 'N', 'A', 'V'):
        output_msp_configure_polling_inav(output);
        break;
    case FW_VARIANT_CONST('B', 'T', 'F', 'L'):
        output_msp_configure_polling_betaflight(output);
        break;
    default:
        LOG_W(TAG, "Unknown fw_variant \"%.4s\"", output->fc.fw_variant);
    }
}

static void output_msp_callback(msp_conn_t *conn, uint16_t cmd, const void *payload, int size, void *callback_data)
{
    // TODO: Stop if size < 0, indicates error
    output_t *output = callback_data;
    switch (cmd)
    {
    case MSP_FC_VARIANT:
        if (size == 4)
        {
            memcpy(output->fc.fw_variant, payload, size);
            // Request version
            MSP_SEND_REQ(output, MSP_FC_VERSION);
        }
        break;
    case MSP_FC_VERSION:
        if (size == 3)
        {
            memcpy(output->fc.fw_version, payload, size);
            output_msp_configure_polling(output);
        }
        break;
    case MSP_NAME:
        OUTPUT_TELEMETRY_UPDATE_STRING(output, TELEMETRY_ID_CRAFT_NAME, payload, size);
        if (output->craft_name_setting && size > 0)
        {
            // The string is not null terminated, so we need to ammend it
            char sval[SETTING_STRING_BUFFER_SIZE];
            size_t val_size = MIN(sizeof(sval) - 1, size);
            memcpy(sval, payload, val_size);
            sval[val_size] = '\0';
            setting_set_string(output->craft_name_setting, sval);
        }
        break;
    case MSP_RSSI_CONFIG:
        if (size == 1)
        {
            output->fc.rssi_channel = *((const uint8_t *)payload);
        }
        break;
    }
}

static void output_msp_poll(output_t *output, time_micros_t now)
{
    if (output->fc.next_fw_update < now)
    {
        MSP_SEND_REQ(output, MSP_FC_VARIANT);
        output->fc.next_fw_update = now + OUTPUT_FC_MSP_UPDATE_INTERVAL;
    }
    for (int ii = 0; ii < OUTPUT_FC_MAX_NUM_POLLS; ii++)
    {
        time_micros_t interval = output->fc.polls[ii].interval;
        if (interval == 0)
        {
            // Note we don't break because we might support polls that can be
            // enabled or disabled depending on other polls (e.g. GPS features)
            continue;
        }
        if (output->fc.polls[ii].next_poll < now)
        {
            MSP_SEND_REQ(output, output->fc.polls[ii].cmd);
            output->fc.polls[ii].next_poll = now + interval;
        }
    }
}

bool output_open(rc_data_t *data, output_t *output, void *config)
{
    bool is_open = false;
    if (output)
    {
        failsafe_init(&output->failsafe);
        rc_data_reset_output(data);
        output->rc_data = data;
        memset(&output->fc, 0, sizeof(output_fc_t));
        output->telemetry_updated = telemetry_updated_callback;
        output->telemetry_calculate = telemetry_calculate_callback;
        output->craft_name_setting = NULL;
        if (!output->is_open && output->vtable.open)
        {
            is_open = output->is_open = output->vtable.open(output, config);
            if (is_open)
            {
                settings_add_listener(output_setting_changed, output);
            }
        }
    }
    return is_open;
}

bool output_update(output_t *output, time_micros_t now)
{
    /*
        TODO: Call update only when needed. Make the output
        declare its minimum interval between updates and a
        maximum (FC might expect a packet from time to time)
        and call it only when needed.
    */
    bool updated = false;
    if (output)
    {
        // Update RC control
        if (output->vtable.update)
        {
            if (output->next_update < now)
            {
                uint16_t channel_value;
                control_channel_t *rssi_channel = NULL;
                if (output->fc.rssi_channel > 0)
                {
                    rssi_channel = &output->rc_data->channels[output->fc.rssi_channel - 1];
                    uint8_t lq = TELEMETRY_GET_I8(output->rc_data, TELEMETRY_ID_RX_LINK_QUALITY);
                    lq = MIN(MAX(0, lq), 100);
                    channel_value = rssi_channel->value;
                    rssi_channel->value = RC_CHANNEL_VALUE_FROM_PERCENTAGE(lq);
                }
                updated = output->vtable.update(output, output->rc_data, now);
                if (updated)
                {
                    output->next_update = now + output->min_update_interval;
                }
                // Restore the channel data we ovewrote
                if (rssi_channel)
                {
                    rssi_channel->value = channel_value;
                }
            }
            failsafe_update(&output->failsafe, time_micros_now());
        }
        // Read MSP transport responses (if any)
        if (msp_io_is_connected(&output->msp))
        {
            // make sure we don't double poll via MSP by polling
            // in the air outputs.
            if (!OUTPUT_HAS_FLAG(output, OUTPUT_FLAG_REMOTE))
            {
                output_msp_poll(output, now);
            }
            msp_io_update(&output->msp);
        }
    }
    return updated;
}

void output_close(output_t *output, void *config)
{
    if (output && output->is_open && output->vtable.close)
    {
        output->vtable.close(output, config);
        output->is_open = false;
        settings_remove_listener(output_setting_changed, output);
    }
}
