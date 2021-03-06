#include <stdlib.h>
#include <string.h>

#include <hal/log.h>

#include "air/air_lora.h"

#include "config/config.h"

#include "msp/msp_serial.h"

#include "platform/pins.h"
#include "platform/system.h"
#include "platform/storage.h"

#include "ui/ui.h"
#include "ui/screen.h"

#include "util/macros.h"
#include "util/version.h"

#include "settings.h"

static const char *TAG = "Settings";

// clang-format off
#define NO_VALUE {0}
#define BOOL(v) {.u8 = (v ? 1 : 0)}
#define U8(v) {.u8 = v}
#define U16(v) {.u16 = v}
#define STR(v) {.s = v}
// clang-format on

static char settings_string_storage[2][SETTING_STRING_BUFFER_SIZE];

#define SETTING_SHOW_IF(c) ((c) ? SETTING_VISIBILITY_SHOW : SETTING_VISIBILITY_HIDE)

static const char *off_on_table[] = {"Off", "On"};
static const char *no_yes_table[] = {"No", "Yes"};
static char pin_number_names[PIN_USABLE_COUNT][3];
static const char *pin_names[PIN_USABLE_COUNT];

#define FOLDER(k, n, id, p, fn) \
    (setting_t) { .key = k, .name = n, .type = SETTING_TYPE_FOLDER, .flags = SETTING_FLAG_READONLY | SETTING_FLAG_EPHEMERAL, .folder = p, .def_val = U8(id), .val = U8(id), .data = fn }
#define STRING_SETTING(k, n, p) \
    (setting_t) { .key = k, .name = n, .type = SETTING_TYPE_STRING, .folder = p }
#define FLAGS_STRING_SETTING(k, n, f, p, v, d) \
    (setting_t) { .key = k, .name = n, .type = SETTING_TYPE_STRING, .flags = f, .folder = p, .val = STR(v), .data = d }
#define RO_STRING_SETTING(k, n, p, v) \
    (setting_t) { .key = k, .name = n, .type = SETTING_TYPE_STRING, .flags = SETTING_FLAG_READONLY, .folder = p, .val = STR(v) }
#define U8_SETTING(k, n, f, p, mi, ma, def) \
    (setting_t) { .key = k, .name = n, .type = SETTING_TYPE_U8, .flags = f, .folder = p, .min = U8(mi), .max = U8(ma), .def_val = U8(def) }
#define U8_MAP_SETTING_UNIT(k, n, f, p, m, u, def) \
    (setting_t) { .key = k, .name = n, .type = SETTING_TYPE_U8, .flags = f | SETTING_FLAG_NAME_MAP, .val_names = m, .unit = u, .folder = p, .min = U8(0), .max = U8(ARRAY_COUNT(m) - 1), .def_val = U8(def) }
#define U8_MAP_SETTING(k, n, f, p, m, def) U8_MAP_SETTING_UNIT(k, n, f, p, m, NULL, def)
#define BOOL_SETTING(k, n, f, p, def) U8_MAP_SETTING(k, n, f, p, off_on_table, def ? 1 : 0)
#define BOOL_YN_SETTING(k, n, f, p, def) U8_MAP_SETTING(k, n, f, p, no_yes_table, def ? 1 : 0)

#define CMD_SETTING(k, n, p, f, c_fl) U8_SETTING(k, n, f | SETTING_FLAG_EPHEMERAL | SETTING_FLAG_CMD, p, 0, 0, c_fl)

#define PIN_SETTING(k, n, p, def) U8_MAP_SETTING(k, n, 0, p, pin_names, def)

#define RX_FOLDER_ID(n) (0xFF - n)

#define RX_KEY(k, n) (k #n)
#define RX_STRING_SETTING(k, n, p, f) FLAGS_STRING_SETTING(k, n, SETTING_FLAG_READONLY | SETTING_FLAG_DYNAMIC, p, NULL, f)
#define RX_CMD(k, n, p) CMD_SETTING(k, n, p, 0, SETTING_CMD_FLAG_CONFIRM)

#define RX_ENSURE_COUNT(n) _Static_assert(CONFIG_MAX_PAIRED_RX == n, "invalid CONFIG_MAX_PAIRED_RX vs settings")

#define RX_FOLDER(n)                                                                                                            \
    FOLDER(RX_KEY(SETTING_KEY_RECEIVERS_RX_PREFIX, n), "Receiver #" #n, RX_FOLDER_ID(n), FOLDER_ID_RECEIVERS, NULL),            \
        RX_STRING_SETTING(RX_KEY(SETTING_KEY_RECEIVERS_RX_NAME_PREFIX, n), "Name", RX_FOLDER_ID(n), setting_format_rx_name),    \
        RX_STRING_SETTING(RX_KEY(SETTING_KEY_RECEIVERS_RX_ADDR_PREFIX, n), "Address", RX_FOLDER_ID(n), setting_format_rx_addr), \
        RX_CMD(RX_KEY(SETTING_KEY_RECEIVERS_RX_SELECT_PREFIX, n), "Select", RX_FOLDER_ID(n)),                                   \
        RX_CMD(RX_KEY(SETTING_KEY_RECEIVERS_RX_DELETE_PREFIX, n), "Delete", RX_FOLDER_ID(n))

#define SETTINGS_STORAGE_KEY "settings"

typedef enum {
    SETTING_VISIBILITY_SHOW,
    SETTING_VISIBILITY_HIDE,
    SETTING_VISIBILITY_MOVE_CONTENTS_TO_PARENT,
} setting_visibility_e;

typedef enum {
    SETTING_DYNAMIC_FORMAT_NAME,
    SETTING_DYNAMIC_FORMAT_VALUE,
} setting_dynamic_format_e;

typedef setting_visibility_e (*setting_visibility_f)(folder_id_e folder, settings_view_e view_id, setting_t *setting);
typedef int (*setting_dynamic_format_f)(char *buf, size_t size, setting_t *setting, setting_dynamic_format_e fmt);

static setting_visibility_e setting_visibility_root(folder_id_e folder, settings_view_e view_id, setting_t *setting)
{
    if (SETTING_IS(setting, SETTING_KEY_RC_MODE))
    {
#if defined(USE_TX_SUPPORT) && defined(USE_RX_SUPPORT)
        return SETTING_SHOW_IF(view_id != SETTINGS_VIEW_CRSF_INPUT);
#else
        return SETTING_VISIBILITY_HIDE;
#endif
    }
    if (SETTING_IS(setting, SETTING_KEY_TX))
    {
        return config_get_rc_mode() == RC_MODE_TX ? SETTING_VISIBILITY_SHOW : SETTING_VISIBILITY_HIDE;
    }
    if (SETTING_IS(setting, SETTING_KEY_RX))
    {
        return config_get_rc_mode() == RC_MODE_RX ? SETTING_VISIBILITY_SHOW : SETTING_VISIBILITY_HIDE;
    }
    if (SETTING_IS(setting, SETTING_KEY_SCREEN))
    {
        return SETTING_SHOW_IF(view_id == SETTINGS_VIEW_MENU && system_has_flag(SYSTEM_FLAG_SCREEN));
    }
    if (SETTING_IS(setting, SETTING_KEY_RECEIVERS))
    {
        return SETTING_SHOW_IF(config_get_rc_mode() == RC_MODE_TX);
    }
    if (SETTING_IS(setting, SETTING_KEY_DEVICES))
    {
        return SETTING_SHOW_IF(view_id == SETTINGS_VIEW_MENU && config_get_rc_mode() == RC_MODE_TX);
    }
    if (SETTING_IS(setting, SETTING_KEY_POWER_OFF))
    {
        return SETTING_SHOW_IF(config_get_rc_mode() == RC_MODE_RX);
    }
    return SETTING_VISIBILITY_SHOW;
}

static setting_visibility_e setting_visibility_tx(folder_id_e folder, settings_view_e view_id, setting_t *setting)
{
    if (SETTING_IS(setting, SETTING_KEY_TX_INPUT))
    {
        return SETTING_SHOW_IF(view_id != SETTINGS_VIEW_CRSF_INPUT);
    }
    if (SETTING_IS(setting, SETTING_KEY_TX_CRSF_PIN))
    {
        return SETTING_SHOW_IF(view_id != SETTINGS_VIEW_CRSF_INPUT && config_get_input_type() == TX_INPUT_CRSF);
    }
    return SETTING_VISIBILITY_SHOW;
}

static setting_visibility_e setting_visibility_rx(folder_id_e folder, settings_view_e view_id, setting_t *setting)
{
    if (SETTING_IS(setting, SETTING_KEY_RX_SBUS_PIN) || SETTING_IS(setting, SETTING_KEY_RX_SBUS_INVERTED) ||
        SETTING_IS(setting, SETTING_KEY_RX_SPORT_PIN) || SETTING_IS(setting, SETTING_KEY_RX_SPORT_INVERTED))
    {
        return SETTING_SHOW_IF(config_get_output_type() == RX_OUTPUT_SBUS_SPORT);
    }

    if (SETTING_IS(setting, SETTING_KEY_RX_MSP_TX_PIN) || SETTING_IS(setting, SETTING_KEY_RX_MSP_RX_PIN) ||
        SETTING_IS(setting, SETTING_KEY_RX_MSP_BAUDRATE))
    {
        return SETTING_SHOW_IF(config_get_output_type() == RX_OUTPUT_MSP);
    }

    if (SETTING_IS(setting, SETTING_KEY_RX_CRSF_TX_PIN) || SETTING_IS(setting, SETTING_KEY_RX_CRSF_RX_PIN))
    {
        return SETTING_SHOW_IF(config_get_output_type() == RX_OUTPUT_CRSF);
    }

    if (SETTING_IS(setting, SETTING_KEY_RX_FPORT_TX_PIN) ||
        SETTING_IS(setting, SETTING_KEY_RX_FPORT_RX_PIN) ||
        SETTING_IS(setting, SETTING_KEY_RX_FPORT_INVERTED))
    {
        return SETTING_SHOW_IF(config_get_output_type() == RX_OUTPUT_FPORT);
    }

    return SETTING_VISIBILITY_SHOW;
}

static setting_visibility_e setting_visibility_receivers(folder_id_e folder, settings_view_e view_id, setting_t *setting)
{
    int rx_num = setting_receiver_get_rx_num(setting);
    return SETTING_SHOW_IF(config_get_paired_rx_at(NULL, rx_num));
}

static int setting_format_rx_name(char *buf, size_t size, setting_t *setting, setting_dynamic_format_e fmt)
{
    if (fmt == SETTING_DYNAMIC_FORMAT_VALUE)
    {
        int rx_num = setting_receiver_get_rx_num(setting);
        air_pairing_t pairing;
        if (!config_get_paired_rx_at(&pairing, rx_num) || !config_get_air_name(buf, size, &pairing.addr))
        {
            strncpy(buf, "None", size);
        }
        return strlen(buf) + 1;
    }
    return 0;
}

static int setting_format_rx_addr(char *buf, size_t size, setting_t *setting, setting_dynamic_format_e fmt)
{
    if (fmt == SETTING_DYNAMIC_FORMAT_VALUE)
    {
        int rx_num = setting_receiver_get_rx_num(setting);
        air_pairing_t pairing;
        if (config_get_paired_rx_at(&pairing, rx_num))
        {
            air_addr_format(&pairing.addr, buf, size);
        }
        else
        {
            strncpy(buf, "None", size);
        }
        return strlen(buf) + 1;
    }
    return 0;
}

#if defined(USE_TX_SUPPORT) && defined(USE_RX_SUPPORT)
static const char *mode_table[] = {"TX", "RX"};
#endif

static const char *lora_band_table[] = {
#if defined(LORA_BAND_433)
    "433MHz",
#endif
#if defined(LORA_BAND_868)
    "868MHz",
#endif
#if defined(LORA_BAND_915)
    "915MHz",
#endif
};
static const char *tx_input_table[] = {"CRSF", "Test"};
static const char *tx_rf_power_table[] = {"Auto", "1mw", "10mw", "25mw", "50mw" /*, "100mw"*/};
_Static_assert(ARRAY_COUNT(tx_rf_power_table) == TX_RF_POWER_LAST - TX_RF_POWER_FIRST + 1, "tx_rf_power_table invalid");
static const char *rx_output_table[] = {"SBUS/Smartport", "MSP", "CRSF", "FPort"};
static const char *msp_baudrate_table[] = {"115200"};
static const char *screen_orientation_table[] = {"Horizontal", "Horizontal (buttons at the right)", "Vertical", "Vertical (buttons on top)"};
static const char *screen_brightness_table[] = {"Low", "Medium", "High"};
static const char *screen_autopoweroff_table[] = {"Disabled", "30 sec", "1 min", "5 min", "10 min"};

static const char *view_crsf_input_tx_settings[] = {
    "",
    SETTING_KEY_BIND,
    SETTING_KEY_TX,
    SETTING_KEY_TX_RF_POWER,
    SETTING_KEY_TX_PILOT_NAME,
    SETTING_KEY_ABOUT,
    SETTING_KEY_ABOUT_VERSION,
    SETTING_KEY_ABOUT_BUILD_DATE,
};

static setting_t settings[] = {
    FOLDER("", "Settings", FOLDER_ID_ROOT, 0, setting_visibility_root),
#if defined(USE_TX_SUPPORT) && defined(USE_RX_SUPPORT)
    U8_MAP_SETTING(SETTING_KEY_RC_MODE, "RC Mode", 0, FOLDER_ID_ROOT, mode_table, RC_MODE_TX),
#elif defined(USE_TX_SUPPORT)
    U8_SETTING(SETTING_KEY_RC_MODE, NULL, SETTING_FLAG_READONLY, FOLDER_ID_ROOT, RC_MODE_TX, RC_MODE_TX, RC_MODE_TX),
#elif defined(USE_RX_SUPPORT)
    U8_SETTING(SETTING_KEY_RC_MODE, NULL, SETTING_FLAG_READONLY, FOLDER_ID_ROOT, RC_MODE_TX, RC_MODE_TX, RC_MODE_TX),
#endif
    BOOL_SETTING(SETTING_KEY_BIND, "Bind", SETTING_FLAG_EPHEMERAL, FOLDER_ID_ROOT, false),
    U8_MAP_SETTING(SETTING_KEY_LORA_BAND, "LoRa Band", 0, FOLDER_ID_ROOT, lora_band_table, AIR_LORA_BAND_DEFAULT),

    FOLDER(SETTING_KEY_TX, "TX", FOLDER_ID_TX, FOLDER_ID_ROOT, setting_visibility_tx),
    U8_MAP_SETTING(SETTING_KEY_TX_RF_POWER, "Power", 0, FOLDER_ID_TX, tx_rf_power_table, TX_RF_POWER_DEFAULT),
    STRING_SETTING(SETTING_KEY_TX_PILOT_NAME, "Pilot Name", FOLDER_ID_TX),
    U8_MAP_SETTING(SETTING_KEY_TX_INPUT, "Input", 0, FOLDER_ID_TX, tx_input_table, TX_INPUT_FIRST),
    PIN_SETTING(SETTING_KEY_TX_CRSF_PIN, "CRSF Pin", FOLDER_ID_TX, PIN_DEFAULT_TX_IDX),

    FOLDER(SETTING_KEY_RX, "RX", FOLDER_ID_RX, FOLDER_ID_ROOT, setting_visibility_rx),
    BOOL_YN_SETTING(SETTING_KEY_RX_AUTO_CRAFT_NAME, "Auto Craft Name", 0, FOLDER_ID_RX, true),
    STRING_SETTING(SETTING_KEY_RX_CRAFT_NAME, "Craft Name", FOLDER_ID_RX),
    U8_MAP_SETTING(SETTING_KEY_RX_OUTPUT, "Output", 0, FOLDER_ID_RX, rx_output_table, RX_OUTPUT_MSP),

    PIN_SETTING(SETTING_KEY_RX_SBUS_PIN, "SBUS Pin", FOLDER_ID_RX, PIN_DEFAULT_TX_IDX),
    BOOL_YN_SETTING(SETTING_KEY_RX_SBUS_INVERTED, "SBUS Inverted", 0, FOLDER_ID_RX, false),
    PIN_SETTING(SETTING_KEY_RX_SPORT_PIN, "S.Port Pin", FOLDER_ID_RX, PIN_DEFAULT_RX_IDX),
    BOOL_YN_SETTING(SETTING_KEY_RX_SPORT_INVERTED, "S.Port Inverted", 0, FOLDER_ID_RX, false),

    PIN_SETTING(SETTING_KEY_RX_MSP_TX_PIN, "MSP TX Pin", FOLDER_ID_RX, PIN_DEFAULT_TX_IDX),
    PIN_SETTING(SETTING_KEY_RX_MSP_RX_PIN, "MSP RX Pin", FOLDER_ID_RX, PIN_DEFAULT_RX_IDX),
    U8_MAP_SETTING(SETTING_KEY_RX_MSP_BAUDRATE, "MSP Baudrate", 0, FOLDER_ID_RX, msp_baudrate_table, MSP_SERIAL_BAUDRATE_FIRST),

    PIN_SETTING(SETTING_KEY_RX_CRSF_TX_PIN, "CRSF TX Pin", FOLDER_ID_RX, PIN_DEFAULT_TX_IDX),
    PIN_SETTING(SETTING_KEY_RX_CRSF_RX_PIN, "CRSF RX Pin", FOLDER_ID_RX, PIN_DEFAULT_RX_IDX),

    PIN_SETTING(SETTING_KEY_RX_FPORT_TX_PIN, "FPort TX Pin", FOLDER_ID_RX, PIN_DEFAULT_TX_IDX),
    PIN_SETTING(SETTING_KEY_RX_FPORT_RX_PIN, "FPort RX Pin", FOLDER_ID_RX, PIN_DEFAULT_RX_IDX),
    BOOL_YN_SETTING(SETTING_KEY_RX_FPORT_INVERTED, "FPort Inverted", 0, FOLDER_ID_RX, false),

    FOLDER(SETTING_KEY_SCREEN, "Screen", FOLDER_ID_SCREEN, FOLDER_ID_ROOT, NULL),
    U8_MAP_SETTING(SETTING_KEY_SCREEN_ORIENTATION, "Orientation", 0, FOLDER_ID_SCREEN, screen_orientation_table, SCREEN_ORIENTATION_DEFAULT),
    U8_MAP_SETTING(SETTING_KEY_SCREEN_BRIGHTNESS, "Brightness", 0, FOLDER_ID_SCREEN, screen_brightness_table, SCREEN_BRIGHTNESS_DEFAULT),
    U8_MAP_SETTING(SETTING_KEY_SCREEN_AUTO_OFF, "Auto Off", 0, FOLDER_ID_SCREEN, screen_autopoweroff_table, UI_SCREEN_AUTOOFF_DEFAULT),

    FOLDER(SETTING_KEY_RECEIVERS, "Receivers", FOLDER_ID_RECEIVERS, FOLDER_ID_ROOT, setting_visibility_receivers),

    RX_FOLDER(0),
    RX_FOLDER(1),
    RX_FOLDER(2),
    RX_FOLDER(3),
#if CONFIG_MAX_PAIRED_RX > 4
    RX_FOLDER(4),
    RX_FOLDER(5),
    RX_FOLDER(6),
    RX_FOLDER(7),
    RX_FOLDER(8),
    RX_FOLDER(9),
    RX_FOLDER(10),
    RX_FOLDER(11),
    RX_FOLDER(12),
    RX_FOLDER(13),
    RX_FOLDER(14),
    RX_FOLDER(15),
#if CONFIG_MAX_PAIRED_RX > 16
    RX_FOLDER(16),
    RX_FOLDER(17),
    RX_FOLDER(18),
    RX_FOLDER(19),
    RX_FOLDER(20),
    RX_FOLDER(21),
    RX_FOLDER(22),
    RX_FOLDER(23),
    RX_FOLDER(24),
    RX_FOLDER(25),
    RX_FOLDER(26),
    RX_FOLDER(27),
    RX_FOLDER(28),
    RX_FOLDER(29),
    RX_FOLDER(30),
    RX_FOLDER(31),
#endif
#endif

    FOLDER(SETTING_KEY_DEVICES, "Other Devices", FOLDER_ID_DEVICES, FOLDER_ID_ROOT, NULL),

    CMD_SETTING(SETTING_KEY_POWER_OFF, "Power Off", FOLDER_ID_ROOT, 0, SETTING_CMD_FLAG_CONFIRM),

    FOLDER(SETTING_KEY_ABOUT, "About", FOLDER_ID_ABOUT, FOLDER_ID_ROOT, NULL),
    RO_STRING_SETTING(SETTING_KEY_ABOUT_VERSION, "Version", FOLDER_ID_ABOUT, SOFTWARE_VERSION),
    RO_STRING_SETTING(SETTING_KEY_ABOUT_BUILD_DATE, "Build Date", FOLDER_ID_ABOUT, __DATE__),
    RO_STRING_SETTING(SETTING_KEY_ABOUT_BUILD_DATE, "Board", FOLDER_ID_ABOUT, BOARD_NAME),
};

#if CONFIG_MAX_PAIRED_RX != 4 && CONFIG_MAX_PAIRED_RX != 16 && CONFIG_MAX_PAIRED_RX != 32
#error Adjust RX_FOLDER(n) settings
#endif

_Static_assert(SETTING_COUNT == ARRAY_COUNT(settings), "SETTING_COUNT != ARRAY_COUNT(settings)");

typedef struct settings_listener_s
{
    setting_changed_f callback;
    void *user_data;
} settings_listener_t;

static settings_listener_t listeners[4];
static storage_t storage;

static void map_setting_keys(settings_view_t *view, const char *keys[], int size)
{
    view->count = 0;
    for (int ii = 0; ii < size; ii++)
    {
        int idx;
        ASSERT(settings_get_key_idx(keys[ii], &idx));
        view->indexes[view->count++] = (uint8_t)idx;
    }
}

static void setting_save(const setting_t *setting)
{
    if (setting->flags & SETTING_FLAG_EPHEMERAL)
    {
        return;
    }
    switch (setting->type)
    {
    case SETTING_TYPE_U8:
        storage_set_u8(&storage, setting->key, setting->val.u8);
        break;
        /*
    case SETTING_TYPE_I8:
        storage_set_i8(&storage, setting->key, setting->val.i8);
        break;
    case SETTING_TYPE_U16:
        storage_set_u16(&storage, setting->key, setting->val.u16);
        break;
    case SETTING_TYPE_I16:
        storage_set_i16(&storage, setting->key, setting->val.i16);
        break;
    case SETTING_TYPE_U32:
        storage_set_u32(&storage, setting->key, setting->val.u32);
        break;
    case SETTING_TYPE_I32:
        storage_set_i32(&storage, setting->key, setting->val.i32);
        break;
        */
    case SETTING_TYPE_STRING:
        storage_set_str(&storage, setting->key, setting->val.s);
        break;
    case SETTING_TYPE_FOLDER:
        break;
    }
    storage_commit(&storage);
}

static void setting_changed(const setting_t *setting)
{
    LOG_I(TAG, "Setting %s changed", setting->key);
    for (int ii = 0; ii < ARRAY_COUNT(listeners); ii++)
    {
        if (listeners[ii].callback)
        {
            listeners[ii].callback(setting, listeners[ii].user_data);
        }
    }
    setting_save(setting);
}

static void setting_move(setting_t *setting, int delta)
{
    if (setting->flags & SETTING_FLAG_READONLY)
    {
        return;
    }
    switch (setting->type)
    {
    case SETTING_TYPE_U8:
    {
        uint8_t v;
        if (delta < 0 && setting->val.u8 == 0)
        {
            v = setting->max.u8;
        }
        else if (delta > 0 && setting->val.u8 == setting->max.u8)
        {
            v = 0;
        }
        else
        {
            v = setting->val.u8 + delta;
        }
        if (v != setting->val.u8)
        {
            setting->val.u8 = v;
            setting_changed(setting);
        }
        break;
    }
    default:
        break;
    }
}

void settings_init(void)
{
    storage_init(&storage, SETTINGS_STORAGE_KEY);

    // Initialize the pin names
    for (int ii = 0; ii < PIN_USABLE_COUNT; ii++)
    {
        snprintf(pin_number_names[ii], sizeof(pin_number_names[ii]), "%02d", usable_pin_at(ii));
        pin_names[ii] = pin_number_names[ii];
    }

    unsigned string_storage_index = 0;

    for (int ii = 0; ii < ARRAY_COUNT(settings); ii++)
    {
        bool found = true;
        setting_t *setting = &settings[ii];
        if (setting->flags & SETTING_FLAG_READONLY)
        {
            continue;
        }
        size_t size;
        switch (setting->type)
        {
        case SETTING_TYPE_U8:
            found = storage_get_u8(&storage, setting->key, &setting->val.u8);
            break;
            /*
        case SETTING_TYPE_I8:
            found = storage_get_i8(&storage, setting->key, &setting->val.i8);
            break;
        case SETTING_TYPE_U16:
            found = storage_get_u16(&storage, setting->key, &setting->val.u16);
            break;
        case SETTING_TYPE_I16:
            found = storage_get_i16(&storage, setting->key, &setting->val.i16);
            break;
        case SETTING_TYPE_U32:
            found = storage_get_u32(&storage, setting->key, &setting->val.u32);
            break;
        case SETTING_TYPE_I32:
            found = storage_get_i32(&storage, setting->key, &setting->val.i32);
            break;
            */
        case SETTING_TYPE_STRING:
            assert(string_storage_index < ARRAY_COUNT(settings_string_storage));
            setting->val.s = settings_string_storage[string_storage_index++];
            size = sizeof(settings_string_storage[0]);
            if (!storage_get_str(&storage, setting->key, setting->val.s, &size))
            {
                memset(setting->val.s, 0, SETTING_STRING_BUFFER_SIZE);
            }
            // We can't copy the default_value over to string settings, otherwise
            // the pointer becomes NULL.
            found = true;
            break;
        case SETTING_TYPE_FOLDER:
            break;
        }
        if (!found && !(setting->flags & SETTING_FLAG_CMD))
        {
            memcpy(&setting->val, &setting->def_val, sizeof(setting->val));
        }
    }
}

void settings_add_listener(setting_changed_f callback, void *user_data)
{
    for (int ii = 0; ii < ARRAY_COUNT(listeners); ii++)
    {
        if (!listeners[ii].callback)
        {
            listeners[ii].callback = callback;
            listeners[ii].user_data = user_data;
            return;
        }
    }
    // Must increase listeners size
    UNREACHABLE();
}

void settings_remove_listener(setting_changed_f callback, void *user_data)
{
    for (int ii = 0; ii < ARRAY_COUNT(listeners); ii++)
    {
        if (listeners[ii].callback == callback && listeners[ii].user_data == user_data)
        {
            listeners[ii].callback = NULL;
            listeners[ii].user_data = NULL;
            return;
        }
    }
    // Tried to remove an unexisting listener
    UNREACHABLE();
}

setting_t *settings_get_setting_at(int idx)
{
    return &settings[idx];
}

setting_t *settings_get_key(const char *key)
{
    return settings_get_key_idx(key, NULL);
}

uint8_t settings_get_key_u8(const char *key)
{
    return setting_get_u8(settings_get_key(key));
}

int settings_get_key_pin_num(const char *key)
{
    return setting_get_pin_num(settings_get_key(key));
}

bool settings_get_key_bool(const char *key)
{
    return setting_get_bool(settings_get_key(key));
}

const char *settings_get_key_string(const char *key)
{
    return setting_get_string(settings_get_key(key));
}

setting_t *settings_get_key_idx(const char *key, int *idx)
{
    for (int ii = 0; ii < ARRAY_COUNT(settings); ii++)
    {
        if (STR_EQUAL(key, settings[ii].key))
        {
            if (idx)
            {
                *idx = ii;
            }
            return &settings[ii];
        }
    }
    return NULL;
}

setting_t *settings_get_folder(folder_id_e folder)
{
    for (int ii = 0; ii < ARRAY_COUNT(settings); ii++)
    {
        if (setting_get_folder_id(&settings[ii]) == folder)
        {
            return &settings[ii];
        }
    }
    return NULL;
}

bool settings_is_folder_visible(settings_view_e view_id, folder_id_e folder)
{
    setting_t *setting = settings_get_folder(folder);
    return setting && setting_is_visible(view_id, setting->key);
}

bool setting_is_visible(settings_view_e view_id, const char *key)
{
    setting_t *setting = settings_get_key(key);
    if (setting)
    {
        setting_visibility_e visibility = SETTING_VISIBILITY_SHOW;
        if (setting->folder)
        {
            setting_t *folder = NULL;
            for (int ii = 0; ii < ARRAY_COUNT(settings); ii++)
            {
                if (setting_get_folder_id(&settings[ii]) == setting->folder)
                {
                    folder = &settings[ii];
                    break;
                }
            }
            if (folder && folder->data)
            {
                setting_visibility_f vf = folder->data;
                visibility = vf(setting_get_folder_id(folder), view_id, setting);
            }
        }
        return visibility != SETTING_VISIBILITY_HIDE;
    }
    return false;
}

folder_id_e setting_get_folder_id(const setting_t *setting)
{
    if (setting->type == SETTING_TYPE_FOLDER)
    {
        return setting->val.u8;
    }
    return 0;
}

folder_id_e setting_get_parent_folder_id(const setting_t *setting)
{
    return setting->folder;
}

int32_t setting_get_min(const setting_t *setting)
{
    switch (setting->type)
    {
    case SETTING_TYPE_U8:
        return setting->min.u8;
    case SETTING_TYPE_STRING:
        break;
    case SETTING_TYPE_FOLDER:
        break;
    }
    return 0;
}

int32_t setting_get_max(const setting_t *setting)
{
    switch (setting->type)
    {
    case SETTING_TYPE_U8:
        return setting->max.u8;
    case SETTING_TYPE_STRING:
        break;
    case SETTING_TYPE_FOLDER:
        break;
    }
    return 0;
}

int32_t setting_get_default(const setting_t *setting)
{
    switch (setting->type)
    {
    case SETTING_TYPE_U8:
        return setting->def_val.u8;
    case SETTING_TYPE_STRING:
        break;
    case SETTING_TYPE_FOLDER:
        break;
    }
    return 0;
}

uint8_t setting_get_u8(const setting_t *setting)
{
    assert(setting->type == SETTING_TYPE_U8);
    return setting->val.u8;
}

// Commands need special processing when used via setting values.
// This is only used when exposing the settings via CRSF.
static void setting_set_u8_cmd(setting_t *setting, uint8_t v)
{
    setting_cmd_flag_e cmd_flags = setting_cmd_get_flags(setting);
    switch ((setting_cmd_status_e)v)
    {
    case SETTING_CMD_STATUS_CHANGE:
        if (cmd_flags & SETTING_CMD_FLAG_CONFIRM)
        {
            // Setting needs a confirmation. Change its value to SETTING_CMD_STATUS_ASK_CONFIRM
            // So clients know they need to show the dialog.
            setting->val.u8 = SETTING_CMD_STATUS_ASK_CONFIRM;
            break;
        }
        if (cmd_flags & SETTING_CMD_FLAG_WARNING)
        {
            setting->val.u8 = SETTING_CMD_STATUS_SHOW_WARNING;
            break;
        }
        // TODO: Timeout if the client doesn't commit or discard after some time for
        // SETTING_CMD_FLAG_CONFIRM and SETTING_CMD_FLAG_WARNING

        // No flags. Just run it.
        setting_cmd_exec(setting);
        break;
    case SETTING_CMD_STATUS_COMMIT:
        setting_cmd_exec(setting);
        setting->val.u8 = 0;
        break;
    case SETTING_CMD_STATUS_NONE:
    case SETTING_CMD_STATUS_DISCARD:
        // TODO: If the command shows a warning while it's active,
        // we need to generate a notification when it stops.
        setting->val.u8 = 0;
        break;
    default:
        // TODO: Once we have a timeout, reset it here
        break;
    }
}

void setting_set_u8(setting_t *setting, uint8_t v)
{
    assert(setting->type == SETTING_TYPE_U8);

    if (setting->flags & SETTING_FLAG_CMD)
    {
        setting_set_u8_cmd(setting, v);
        return;
    }

    v = MIN(v, setting->max.u8);
    v = MAX(v, setting->min.u8);
    if ((setting->flags & SETTING_FLAG_READONLY) == 0 && setting->val.u8 != v)
    {
        setting->val.u8 = v;
        setting_changed(setting);
    }
}

int setting_get_pin_num(setting_t *setting)
{
    assert(setting->val_names == pin_names);
    return usable_pin_at(setting->val.u8);
}

bool setting_get_bool(const setting_t *setting)
{
    assert(setting->type == SETTING_TYPE_U8);
    return setting->val.u8;
}

void setting_set_bool(setting_t *setting, bool v)
{
    assert(setting->type == SETTING_TYPE_U8);
    uint8_t val = v ? 1 : 0;
    if (val != setting->val.u8)
    {
        setting->val.u8 = val;
        setting_changed(setting);
    }
}

const char *setting_get_string(setting_t *setting)
{
    assert(setting->type == SETTING_TYPE_STRING);
    assert((setting->flags & SETTING_FLAG_DYNAMIC) == 0);
    return setting->val.s;
}

void setting_set_string(setting_t *setting, const char *s)
{
    assert(setting->type == SETTING_TYPE_STRING);
    if (!STR_EQUAL(setting->val.s, s))
    {
        strlcpy(setting->val.s, s, SETTING_STRING_BUFFER_SIZE);
        setting_changed(setting);
    }
}

int settings_get_count(void)
{
    return ARRAY_COUNT(settings);
}

const char *setting_map_name(setting_t *setting, uint8_t val)
{
    if (setting->flags & SETTING_FLAG_NAME_MAP && setting->val_names)
    {
        if (val <= setting->max.u8)
        {
            return setting->val_names[val];
        }
    }
    return NULL;
}

void setting_format_name(char *buf, size_t size, setting_t *setting)
{
    if (setting->name)
    {
        strncpy(buf, setting->name, size);
    }
    else
    {
        buf[0] = '\0';
    }
}

void setting_format(char *buf, size_t size, setting_t *setting)
{
    char name[SETTING_NAME_BUFFER_SIZE];

    setting_format_name(name, sizeof(name), setting);

    if (setting->type == SETTING_TYPE_FOLDER)
    {
        snprintf(buf, size, "%s >>", name);
        return;
    }
    if (setting->flags & SETTING_FLAG_CMD)
    {
        // Commands don't show their values
        snprintf(buf, size, "%s \xAC", name);
        return;
    }
    char value[64];
    setting_format_value(value, sizeof(value), setting);
    snprintf(buf, size, "%s: %s", name, value);
}

void setting_format_value(char *buf, size_t size, setting_t *setting)
{
    if (setting->flags & SETTING_FLAG_NAME_MAP)
    {
        const char *name = setting_map_name(setting, setting->val.u8);
        snprintf(buf, size, "%s", name);
    }
    else
    {
        switch (setting->type)
        {
        case SETTING_TYPE_U8:
            snprintf(buf, size, "%u", setting->val.u8);
            break;
            /*
        case SETTING_TYPE_I8:
            snprintf(buf, size, "%d", setting->val.i8);
            break;
        case SETTING_TYPE_U16:
            snprintf(buf, size, "%u", setting->val.u16);
            break;
        case SETTING_TYPE_I16:
            snprintf(buf, size, "%d", setting->val.i16);
            break;
        case SETTING_TYPE_U32:
            snprintf(buf, size, "%u", setting->val.u32);
            break;
        case SETTING_TYPE_I32:
            snprintf(buf, size, "%d", setting->val.i32);
            break;
            */
        case SETTING_TYPE_STRING:
        {
            const char *value = setting->val.s;
            char value_buf[SETTING_STRING_BUFFER_SIZE];
            if (setting->flags & SETTING_FLAG_DYNAMIC)
            {
                if (setting->data)
                {
                    setting_dynamic_format_f format_f = setting->data;
                    if (format_f(value_buf, sizeof(value_buf), setting, SETTING_DYNAMIC_FORMAT_VALUE) > 0)
                    {
                        value = value_buf;
                    }
                }
            }
            snprintf(buf, size, "%s", value ?: "<null>");
            break;
        }
        case SETTING_TYPE_FOLDER:
            break;
        }
    }
    if (setting->unit)
    {
        strlcat(buf, setting->unit, size);
    }
}

setting_cmd_flag_e setting_cmd_get_flags(const setting_t *setting)
{
    if (setting->flags & SETTING_FLAG_CMD)
    {
        // CMD flags are stored in the default value
        return setting->def_val.u8;
    }
    return 0;
}

bool setting_cmd_exec(const setting_t *setting)
{
    setting_changed(setting);
    return true;
}

int setting_receiver_get_rx_num(const setting_t *setting)
{
    if (strstr(setting->key, SETTING_KEY_RECEIVERS_PREFIX))
    {
        folder_id_e folder;
        if (setting->type == SETTING_TYPE_FOLDER)
        {
            folder = setting_get_folder_id(setting);
        }
        else
        {
            folder = setting_get_parent_folder_id(setting);
        }
        return RX_FOLDER_ID(folder);
    }
    return -1;
}

void setting_increment(setting_t *setting)
{
    setting_move(setting, 1);
}

void setting_decrement(setting_t *setting)
{
    setting_move(setting, -1);
}

static void settings_view_get_actual_folder_view(settings_view_t *view, settings_view_e view_id, folder_id_e folder, bool recursive)
{
    setting_visibility_f visibility_fn = NULL;
    for (int ii = 0; ii < SETTING_COUNT; ii++)
    {
        setting_t *setting = &settings[ii];
        if (setting_get_folder_id(setting) == folder)
        {
            visibility_fn = settings[ii].data;
            continue;
        }
        if (setting_get_parent_folder_id(setting) == folder)
        {
            setting_visibility_e visibility = SETTING_VISIBILITY_SHOW;
            if (visibility_fn)
            {
                visibility = visibility_fn(folder, view_id, setting);
            }
            switch (visibility)
            {
            case SETTING_VISIBILITY_SHOW:
                view->indexes[view->count++] = ii;
                if (recursive && setting->type == SETTING_TYPE_FOLDER)
                {
                    // Include this dir
                    settings_view_get_actual_folder_view(view, view_id, setting_get_folder_id(setting), recursive);
                }
                break;
            case SETTING_VISIBILITY_HIDE:
                break;
            case SETTING_VISIBILITY_MOVE_CONTENTS_TO_PARENT:
                // Add the settings in this folder to its parent
                ASSERT(setting->type == SETTING_TYPE_FOLDER);
                settings_view_get_actual_folder_view(view, view_id, setting_get_folder_id(setting), recursive);
                break;
            }
        }
    }
}

bool settings_view_get_folder_view(settings_view_t *view, settings_view_e view_id, folder_id_e folder, bool recursive)
{
    view->count = 0;
    settings_view_get_actual_folder_view(view, view_id, folder, recursive);
    return view->count > 0;
}

bool settings_view_get(settings_view_t *view, settings_view_e view_id, folder_id_e folder)
{
    switch (view_id)
    {
    case SETTINGS_VIEW_CRSF_INPUT:
        map_setting_keys(view, view_crsf_input_tx_settings, ARRAY_COUNT(view_crsf_input_tx_settings));
        return true;
    case SETTINGS_VIEW_MENU:
        return settings_view_get_folder_view(view, view_id, folder, false);
    case SETTINGS_VIEW_REMOTE:
        view->count = 0;
        return settings_view_get_folder_view(view, view_id, folder, true);
    }
    return false;
}

setting_t *settings_view_get_setting_at(settings_view_t *view, int idx)
{
    if (idx >= 0 && idx < view->count)
    {
        return &settings[view->indexes[idx]];
    }
    return NULL;
}

int settings_view_get_parent_index(settings_view_t *view, setting_t *setting)
{
    for (int ii = 0; ii < view->count; ii++)
    {
        setting_t *vs = settings_view_get_setting_at(view, ii);
        if (vs->type == SETTING_TYPE_FOLDER && setting_get_folder_id(vs) == setting->folder)
        {
            return ii;
        }
    }
    return -1;
}
