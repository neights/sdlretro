#include "driver_base.h"

#include "logger.h"

#include "buffered_audio.h"
#include "throttle.h"

#include <core.h>

#include <fstream>

#include <cstring>
#include <memory>

namespace drivers {

driver_base *current_driver = nullptr;

driver_base::~driver_base() {
    deinit_internal();

    if (core) {
        core_unload(core);
        core = nullptr;
    }

    current_driver = nullptr;
}

void driver_base::run() {
    for (;;) {
        if (!run_frame()) break;
        core->retro_run();
    }
}

bool RETRO_CALLCONV retro_environment_cb(unsigned cmd, void *data) {
    if (!current_driver) return false;
    return current_driver->env_callback(cmd, data);
}

void RETRO_CALLCONV log_printf(enum retro_log_level level, const char *fmt, ...) {
    va_list l;
    va_start(l, fmt);
    log_vprintf((int)level, fmt, l);
}

static void RETRO_CALLCONV retro_video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    current_driver->video_refresh(data, width, height, pitch);
}

static void RETRO_CALLCONV retro_audio_sample_cb(int16_t left, int16_t right) {
    int16_t samples[2] = {left, right};
    current_driver->get_audio()->write_samples(samples, 2);
}

static size_t RETRO_CALLCONV retro_audio_sample_batch_cb(const int16_t *data, size_t frames) {
    current_driver->get_audio()->write_samples(data, frames * 2);
    return frames;
}

static void RETRO_CALLCONV retro_input_poll_cb() {
    current_driver->input_poll();
}

static int16_t RETRO_CALLCONV retro_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    return current_driver->input_state(port, device, index, id);
}

void driver_base::load_game(const std::string &path) {
    retro_game_info info = {};
    info.path = path.c_str();
    if (!need_fullpath) {
        std::ifstream ifs(path, std::ios_base::binary | std::ios_base::in);
        ifs.seekg(0, std::ios_base::end);
        game_data.resize(ifs.tellg());
        ifs.seekg(0, std::ios_base::beg);
        ifs.read(&game_data[0], game_data.size());
        info.data = &game_data[0];
        info.size = game_data.size();
    }
    if (!core->retro_load_game(&info)) {
        fprintf(stderr, "Unable to load %s\n", path.c_str());
    }

    retro_system_av_info av_info = {};
    core->retro_get_system_av_info(&av_info);
    base_width = av_info.geometry.base_width;
    base_height = av_info.geometry.base_height;
    max_width = av_info.geometry.max_width;
    max_height = av_info.geometry.max_height;
    aspect_ratio = av_info.geometry.aspect_ratio;

    audio->init(false, av_info.timing.sample_rate, av_info.timing.fps);
    frame_throttle->reset(av_info.timing.fps);

    geometry_updated();
}

void driver_base::unload_game() {
    core->retro_unload_game();
}

void driver_base::reset() {
    core->retro_reset();
}

inline void load_variable(std::map<std::string, driver_base::variable_t> &variables, const retro_core_option_definition *def) {
    while (def->key != nullptr) {
        auto &var = variables[def->key];
        var.curr_index = 0;
        var.default_index = 0;
        var.label = def->desc;
        var.info = def->info;
        const auto *opt = def->values;
        while (opt->value != nullptr) {
            if (strcmp(opt->value, def->default_value)==0)
                var.curr_index = var.default_index = var.options.size();
            var.options.emplace_back(std::make_pair(opt->value, opt->label == nullptr ? "" : opt->label));
            ++opt;
        }
        var.visible = true;
        ++def;
    }
}

bool driver_base::env_callback(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_ROTATION:
            break;
        case RETRO_ENVIRONMENT_GET_OVERSCAN:
            *(bool*)data = false;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE:
        case RETRO_ENVIRONMENT_SHUTDOWN:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            break;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char**)data = system_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            pixel_format = (unsigned)*(const enum retro_pixel_format *)data;
            return true;
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
            const auto *inp = (const retro_input_descriptor*)data;
            while (inp->description != nullptr) {
                pads.add_button(inp->port, inp->device, inp->index, inp->id, inp->description);
                ++inp;
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            break;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto *var = (retro_variable *)data;
            auto ite = variables.find(var->key);
            if (ite != variables.end()) {
                var->value = ite->second.options[ite->second.curr_index].first.c_str();
                return true;
            }
            break;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const auto *vars = (const retro_variable*)data;
            variables.clear();
            variables_updated = false;
            while (vars->key != nullptr) {
                std::string value = vars->value;
                auto pos = value.find("; ");
                if (pos != std::string::npos) {
                    auto &var = variables[vars->key];
                    var.curr_index = 0;
                    var.default_index = 0;
                    var.label = value.substr(0, pos);
                    var.info.clear();
                    var.visible = true;
                    pos += 2;
                    for (;;) {
                        int end_pos = value.find('|', pos);
                        if (pos < end_pos)
                            var.options.emplace_back(std::make_pair(value.substr(pos, end_pos - pos), std::string()));
                        if (end_pos == std::string::npos) {
                            break;
                        }
                        pos = end_pos + 1;
                    }
                }
                ++vars;
            }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = variables_updated;
            return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            support_no_game = *(bool*)data;
            break;
        case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
            *(const char**)data = libretro_dir.empty() ? nullptr : libretro_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            break;
        case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
            *(uint64_t*)data = 1ULL << RETRO_DEVICE_JOYPAD;
            return true;
        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
        case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:
            break;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            ((retro_log_callback*)data)->log = log_printf;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        case RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            break;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data = save_dir.empty() ? nullptr : save_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
            break;
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            const auto *memmap = (const retro_memory_map*)data;
            for (unsigned i = 0; i < memmap->num_descriptors; ++i) {
                // TODO: store info of memory map for future use
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const auto *geometry = (const retro_game_geometry*)data;
            base_width = geometry->base_width;
            base_height = geometry->base_height;
            max_width = geometry->max_width;
            max_height = geometry->max_height;
            aspect_ratio = geometry->aspect_ratio;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_USERNAME:
            *(const char**)data = "sdlretro";
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:
        case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        case RETRO_ENVIRONMENT_GET_MIDI_INTERFACE:
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
            break;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            if (data) *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *(unsigned*)data = RETRO_API_VERSION;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
            variables.clear();
            variables_updated = false;
            load_variable(variables, (const retro_core_option_definition*)data);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
            variables.clear();
            variables_updated = false;
            load_variable(variables, ((const retro_core_options_intl*)data)->us);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
            const auto *opt = (const retro_core_option_display*)data;
            auto ite = variables.find(opt->key);
            if (ite != variables.end()) {
                ite->second.visible = opt->visible;
                return true;
            }
            break;
        }
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        default:
            break;
    }
    fprintf(stdout, "Unhandled env: %d\n", cmd & 0xFFFF);
    return false;
}

bool driver_base::load(const std::string &path) {
    core = core_load(path.c_str());
    if (!core) return false;

    current_driver = this;

    init_internal();
    return true;
}

bool driver_base::init_internal() {
    if (inited) return true;

    core->retro_set_environment(retro_environment_cb);

    retro_system_info info = {};
    core->retro_get_system_info(&info);
    need_fullpath = info.need_fullpath;

    core->retro_init();

    core->retro_set_video_refresh(retro_video_refresh_cb);
    core->retro_set_audio_sample(retro_audio_sample_cb);
    core->retro_set_audio_sample_batch(retro_audio_sample_batch_cb);
    core->retro_set_input_poll(retro_input_poll_cb);
    core->retro_set_input_state(retro_input_state_cb);

    if (!init()) {
        core->retro_deinit();
        return false;
    }

    inited = true;
    return true;
}

void driver_base::deinit_internal() {
    if (!inited) return;

    deinit();

    core->retro_deinit();

    /* reset all variables to default value */
    need_fullpath = false;

    pixel_format = 0;
    support_no_game = false;

    base_width = 0;
    base_height = 0;
    max_width = 0;
    max_height = 0;
    aspect_ratio = 0.f;

    game_data.clear();

    variables.clear();
    variables_updated = false;

    inited = false;
}

int16_t driver_base::input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return pad_states;
    return pad_states & (1 << id);
}
driver_base::driver_base() {
    frame_throttle = std::make_unique<throttle>();
}

}