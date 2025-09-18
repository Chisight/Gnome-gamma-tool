// Compile with: gcc -o gamma-tool gamma-tool.c $(pkg-config --cflags --libs glib-2.0 gobject-2.0 colord gio-2.0) -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>    // For pow()
#include <unistd.h>  // For sleep()
#include <glib.h>
#include <colord.h>
#include <gio/gio.h> // Required for GFile

#define N_SAMPLES 256
#define OUR_PREFIX "gamma-tool-"
#define TIMEOUT_SECONDS 4

// A struct to hold our parsed command-line arguments
typedef struct {
    gfloat gamma[3];
    gint temperature;
    gboolean remove_profile;
    gboolean info_mode;
    gint device_index; // -1 means all devices
} AppArgs;

// --- Forward Declarations of Helper Functions ---
static void parse_arguments(int argc, char *argv[], AppArgs *args);
static GList *get_display_devices(CdClient *client);
static void process_device(CdClient *client, CdDevice *device, AppArgs *args);
static void handle_info_mode(CdDevice *device, CdProfile *profile);
static void handle_remove_mode(CdDevice *device, CdProfile *profile);
static void handle_apply_mode(CdClient *client, CdDevice *device, CdProfile *profile, AppArgs *args);
static void generate_vcgt(gfloat gamma[3], gint color_temperature, CdIcc *profile_data);
static CdProfile *create_and_set_sRGB_profile(CdClient *client, CdDevice *device);


/**
 * @brief The main entry point of the gamma-tool program.
 *
 * Orchestrates the entire process: parses arguments, connects to the colord service,
 * discovers all display devices, and then either processes a single targeted device
 * or all of them based on user input.
 *
 * @param argc The number of command-line arguments.
 * @param argv An array of command-line argument strings.
 * @return 0 on success, 1 on failure.
 */
int main(int argc, char *argv[]) {
    AppArgs args;
    parse_arguments(argc, argv, &args);

    // --- Colord Client Setup ---
    GError *error = NULL;
    CdClient *client = cd_client_new();
    if (!cd_client_connect_sync(client, NULL, &error)) {
        g_critical("Failed to connect to colord: %s", error->message);
        g_error_free(error);
        g_object_unref(client);
        return 1;
    }

    // --- Discover Devices ---
    GList *display_devices = get_display_devices(client);
    if (!display_devices) {
        printf("No display devices found.\n");
        g_object_unref(client);
        return 0;
    }

    // --- Process Device(s) ---
    if (args.device_index != -1) {
        // Single device mode
        guint num_devices = g_list_length(display_devices);
        if (args.device_index >= (gint)num_devices) {
            fprintf(stderr, "Error: Invalid device index %d. Only %u devices found (0 to %u).\n",
                    args.device_index, num_devices, num_devices > 0 ? num_devices - 1 : 0);
            g_list_free_full(display_devices, g_object_unref);
            g_object_unref(client);
            return 1;
        }
        CdDevice *device = g_list_nth_data(display_devices, args.device_index);
        process_device(client, device, &args);
    } else {
        // All devices mode
        for (GList *l = display_devices; l != NULL; l = l->next) {
            CdDevice *device = l->data;
            process_device(client, device, &args);
        }
    }

    // --- Final Cleanup ---
    g_list_free_full(display_devices, g_object_unref);
    g_object_unref(client);

    return 0;
}

/**
 * @brief Parses command line arguments and populates the AppArgs struct.
 * Exits if arguments are invalid or if help is requested.
 */
static void parse_arguments(int argc, char *argv[], AppArgs *args) {
    *args = (AppArgs){
        .gamma = {1.0f, 1.0f, 1.0f},
        .temperature = 6500,
        .remove_profile = FALSE,
        .info_mode = FALSE,
        .device_index = -1, // Default to all devices
    };
    const char *gamma_str = "1.0";

    for (int i = 1; i < argc; ++i) {
        if (g_strcmp0(argv[i], "-r") == 0) {
            args->remove_profile = TRUE;
        } else if (g_strcmp0(argv[i], "-i") == 0) {
            args->info_mode = TRUE;
        } else if (g_str_has_prefix(argv[i], "-d")) {
            const char* device_idx_str = NULL;
            if (g_strcmp0(argv[i], "-d") == 0 && (i + 1) < argc) {
                device_idx_str = argv[++i];
            } else if (g_str_has_prefix(argv[i], "-d=")) {
                device_idx_str = argv[i] + 3; // Skip "-d="
            }
            if (device_idx_str) {
                 args->device_index = atoi(device_idx_str);
            }
        } else if (g_str_has_prefix(argv[i], "-g")) {
            if (g_strcmp0(argv[i], "-g") == 0 && (i + 1) < argc) {
                gamma_str = argv[++i];
            } else if (g_str_has_prefix(argv[i], "-g=")) {
                gamma_str = argv[i] + 3; // Skip "-g="
            }
        } else if (g_str_has_prefix(argv[i], "-t")) {
            const char* temp_val_str = NULL;
            if (g_strcmp0(argv[i], "-t") == 0 && (i + 1) < argc) {
                temp_val_str = argv[++i];
            } else if (g_str_has_prefix(argv[i], "-t=")) {
                temp_val_str = argv[i] + 3; // Skip "-t="
            }
            if (temp_val_str) {
                 args->temperature = atoi(temp_val_str);
            }
        }
    }

    gchar **parts = g_strsplit(gamma_str, ":", 3);
    if (parts[0] && !parts[1]) {
        gfloat val = g_ascii_strtod(parts[0], NULL);
        args->gamma[0] = val; args->gamma[1] = val; args->gamma[2] = val;
    } else if (parts[0] && parts[1] && parts[2]) {
        args->gamma[0] = g_ascii_strtod(parts[0], NULL);
        args->gamma[1] = g_ascii_strtod(parts[1], NULL);
        args->gamma[2] = g_ascii_strtod(parts[2], NULL);
    }
    g_strfreev(parts);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s [-d INDEX] [-g R:G:B|G] [-t TEMP] [-r] [-i]\n", argv[0]);
        fprintf(stderr, "  -d INDEX       Target a specific display index (e.g., 0).\n");
        fprintf(stderr, "  -g GAMMA       Target gamma (e.g., 0.8), 1.0 is neutral.\n");
        fprintf(stderr, "  -t TEMPERATURE Target color temperature, 6500 is neutral.\n");
        fprintf(stderr, "  -r             Remove existing profile created by this tool.\n");
        fprintf(stderr, "  -i             Display info about the current profile.\n");
        exit(1);
    }
}

/**
 * @brief Contains the core logic for processing a single device.
 *
 * This function encapsulates the logic that was previously in the main loop.
 * It fetches the current profile for the given device and then delegates to the
 * appropriate handler based on the program's operating mode.
 *
 * @param client (Input) The connected CdClient instance.
 * @param device (Input) The specific display device to process.
 * @param args   (Input) Pointer to the parsed application arguments.
 */
static void process_device(CdClient *client, CdDevice *device, AppArgs *args) {
    printf("\ndevice: %s\n", cd_device_get_id(device));
    GError *error = NULL;

    GPtrArray *profiles = cd_device_get_profiles(device);
    CdProfile *base_profile = NULL;

    if (profiles != NULL && profiles->len > 0) {
        base_profile = g_object_ref(g_ptr_array_index(profiles, 0));
    } else {
        printf("No default profile, using sRGB\n");
        base_profile = create_and_set_sRGB_profile(client, device);
        if (!base_profile) {
            g_warning("Could not set sRGB profile for %s. Skipping.", cd_device_get_id(device));
            if (profiles) g_ptr_array_free(profiles, TRUE);
            return;
        }
    }
    if (profiles) g_ptr_array_free(profiles, TRUE);

    if (!cd_profile_connect_sync(base_profile, NULL, &error)) {
        g_warning("Could not connect to base profile: %s", error->message);
        g_error_free(error); error = NULL;
        g_object_unref(base_profile);
        return;
    }

    if (args->info_mode) {
        handle_info_mode(device, base_profile);
    } else if (args->remove_profile) {
        handle_remove_mode(device, base_profile);
    } else {
        handle_apply_mode(client, device, base_profile, args);
    }

    g_object_unref(base_profile);
}

/**
 * @brief Gets a list of all connected display devices from the colord service.
 * @return A GList of connected CdDevice objects. The caller must free this list.
 */
static GList *get_display_devices(CdClient *client) {
    GError *error = NULL;
    GPtrArray *all_devices = cd_client_get_devices_sync(client, NULL, &error);
    if (error) {
        g_critical("Failed to get devices: %s", error->message);
        g_error_free(error);
        return NULL;
    }
    GList *display_devices = NULL;
    for (guint i = 0; i < all_devices->len; i++) {
        CdDevice *device = g_ptr_array_index(all_devices, i);
        if (!cd_device_connect_sync(device, NULL, &error)) {
            g_warning("Could not connect to device %s: %s", cd_device_get_id(device), error->message);
            g_error_free(error); error = NULL;
            continue;
        }
        if (cd_device_get_kind(device) == CD_DEVICE_KIND_DISPLAY) {
            display_devices = g_list_append(display_devices, g_object_ref(device));
        }
    }
    g_ptr_array_free(all_devices, TRUE);
    return display_devices;
}

/**
 * @brief Handles the -i (info) mode for a single device.
 */
static void handle_info_mode(CdDevice *device, CdProfile *profile) {
    const char *profile_filename = cd_profile_get_filename(profile);
    if (profile_filename == NULL) {
        printf("Current profile has no filename.\n");
        return;
    }
    gchar *basename = g_path_get_basename(profile_filename);
    if (g_str_has_prefix(basename, OUR_PREFIX)) {
        int r, g, b, temp;
        int items = sscanf(basename, "gamma-tool-g%3d%3d%3dt%d-", &r, &g, &b, &temp);
        if (items == 4) {
            printf("gamma: %.2f:%.2f:%.2f\n", r / 100.0f, g / 100.0f, b / 100.0f);
            printf("temperature: %d\n", temp);
        } else {
            printf("Could not parse parameters from profile name: %s\n", basename);
        }
    } else {
        printf("Current profile is not a gamma-tool profile: %s\n", profile_filename);
    }
    g_free(basename);
}

/**
 * @brief Handles the -r (remove) mode for a single device.
 */
static void handle_remove_mode(CdDevice *device, CdProfile *profile) {
    const char *profile_filename = cd_profile_get_filename(profile);
    printf("Current profile is %s\n", profile_filename ? profile_filename : cd_profile_get_id(profile));

    gboolean is_our_profile = FALSE;
    if (profile_filename != NULL) {
        gchar *basename = g_path_get_basename(profile_filename);
        if (g_str_has_prefix(basename, OUR_PREFIX)) {
            is_our_profile = TRUE;
        }
        g_free(basename);
    }
    if (is_our_profile) {
        GError *error = NULL;
        printf("Removing profile from device...\n");
        if (cd_device_remove_profile_sync(device, profile, NULL, &error)) {
            printf("Deleting file %s\n", profile_filename);
            if (remove(profile_filename) != 0) {
                g_warning("Could not delete profile file: %s", profile_filename);
            }
        } else {
            g_warning("Could not remove profile from device: %s", error->message);
            g_error_free(error);
        }
    } else {
        printf("Current profile was not created by this tool. Not removing.\n");
    }
}

/**
 * @brief Handles the default mode: creating and applying a new profile.
 */
static void handle_apply_mode(CdClient *client, CdDevice *device, CdProfile *profile, AppArgs *args) {
    const char *profile_filename = cd_profile_get_filename(profile);
    printf("Current profile is %s\n", profile_filename ? profile_filename : cd_profile_get_id(profile));

    gboolean is_our_profile = FALSE;
    if (profile_filename != NULL) {
        gchar *basename = g_path_get_basename(profile_filename);
        if (g_str_has_prefix(basename, OUR_PREFIX)) {
            is_our_profile = TRUE;
        }
        g_free(basename);
    }
    
    GError *error = NULL;
    CdIcc *profile_data = cd_profile_load_icc(profile, CD_ICC_LOAD_FLAGS_NONE, NULL, &error);
    if (error) {
        g_warning("Could not get ICC data from base profile: %s", error->message);
        g_error_free(error);
        return;
    }
    
    gchar *title = g_strdup_printf("gamma-tool: g=%.2f:%.2f:%.2f t=%d", args->gamma[0], args->gamma[1], args->gamma[2], args->temperature);
    cd_icc_set_description(profile_data, "", title);
    g_free(title);

    gchar *uuid_str = g_uuid_string_random();
    cd_icc_add_metadata(profile_data, "uuid", uuid_str);
    generate_vcgt(args->gamma, args->temperature, profile_data);

    int r = (int)(args->gamma[0] * 100.0f); int g = (int)(args->gamma[1] * 100.0f); int b = (int)(args->gamma[2] * 100.0f);
    gchar *new_basename = g_strdup_printf("%sg%03d%03d%03dt%d-%s.icc",
                                          OUR_PREFIX, r, g, b, args->temperature, uuid_str);
    gchar *icc_dir = g_build_filename(g_get_user_data_dir(), "icc", NULL);
    g_mkdir_with_parents(icc_dir, 0755);
    gchar *new_path = g_build_filename(icc_dir, new_basename, NULL);
    GFile *profile_file = g_file_new_for_path(new_path);
    CdProfile *new_profile = NULL;
    
    if (!cd_icc_save_file(profile_data, profile_file, CD_ICC_SAVE_FLAGS_NONE, NULL, &error)) {
        g_warning("Could not save new profile to %s: %s", new_path, error->message);
        g_error_free(error);
    } else {
        gint64 deadline = g_get_monotonic_time() + TIMEOUT_SECONDS * G_TIME_SPAN_SECOND;
        while (g_get_monotonic_time() < deadline) {
            new_profile = cd_client_find_profile_by_filename_sync(client, new_path, NULL, NULL);
            if (new_profile) break;
            g_main_context_iteration(NULL, FALSE);
            g_usleep(10000);
        }
        if (new_profile && cd_profile_connect_sync(new_profile, NULL, &error)) {
            printf("New profile is %s\n", cd_profile_get_filename(new_profile));
            if (!cd_device_add_profile_sync(device, CD_DEVICE_RELATION_HARD, new_profile, NULL, NULL))
                g_warning("Failed to add new profile to device.");
            if (!cd_device_make_profile_default_sync(device, new_profile, NULL, NULL))
                g_warning("Failed to make new profile default.");
        } else if (new_profile) {
            g_warning("Could not connect to new profile: %s", error->message);
            g_error_free(error);
        } else {
            g_warning("Timed out waiting for colord to detect new profile: %s", new_path);
        }
    }

    if (is_our_profile && new_profile) {
        printf("Removing old profile...\n");
        if (cd_device_remove_profile_sync(device, profile, NULL, &error)) {
            printf("Deleting file %s\n", profile_filename);
            if (remove(profile_filename) != 0) {
                g_warning("Could not delete old profile file: %s", profile_filename);
            }
        } else {
             g_warning("Could not remove old profile from device: %s", error->message);
             g_error_free(error);
        }
    }

    if (new_profile) g_object_unref(new_profile);
    g_object_unref(profile_file);
    g_free(new_basename); g_free(icc_dir); g_free(new_path);
    g_free(uuid_str); g_object_unref(profile_data);
}

/**
 * @brief Generates a Video Card Gamma Table (VCGT) and applies it to an ICC profile.
 */
static void generate_vcgt(gfloat gamma[3], gint color_temperature, CdIcc *profile_data) {
    gfloat gamma_factor[3];
    gamma_factor[0] = 1.0f / gamma[0]; gamma_factor[1] = 1.0f / gamma[1]; gamma_factor[2] = 1.0f / gamma[2];
    CdColorRGB temp_color; GError *error = NULL;
    GPtrArray *vcgt_array = g_ptr_array_new_with_free_func(g_free);
    cd_color_get_blackbody_rgb_full(color_temperature, &temp_color, CD_COLOR_BLACKBODY_FLAG_USE_PLANCKIAN);
    for (gint i = 0; i < N_SAMPLES; i++) {
        CdColorRGB *c = g_new(CdColorRGB, 1);
        gdouble step = (gdouble)i / (N_SAMPLES - 1);
        c->R = temp_color.R * pow(step, gamma_factor[0]);
        c->G = temp_color.G * pow(step, gamma_factor[1]);
        c->B = temp_color.B * pow(step, gamma_factor[2]);
        c->R = CLAMP(c->R, 0.0, 1.0); c->G = CLAMP(c->G, 0.0, 1.0); c->B = CLAMP(c->B, 0.0, 1.0);
        g_ptr_array_add(vcgt_array, c);
    }
    if (!cd_icc_set_vcgt(profile_data, vcgt_array, &error)) {
        g_warning("Failed to set VCGT: %s", error->message);
        g_error_free(error);
    }
    g_ptr_array_free(vcgt_array, TRUE);
}

/**
 * @brief Finds the standard sRGB profile and sets it as the default for a device.
 */
static CdProfile *create_and_set_sRGB_profile(CdClient *client, CdDevice *device) {
    GError *error = NULL;
    CdProfile *profile = cd_client_find_profile_by_filename_sync(client, "sRGB.icc", NULL, &error);
    if (!profile) {
        g_warning("Failed to find sRGB.icc profile: %s", error ? error->message : "Not found");
        if(error) g_error_free(error);
        return NULL;
    }
    if (!cd_profile_connect_sync(profile, NULL, &error)) {
        g_warning("Could not connect to sRGB profile: %s", error->message);
        g_error_free(error); g_object_unref(profile); return NULL;
    }
    if (!cd_device_add_profile_sync(device, CD_DEVICE_RELATION_HARD, profile, NULL, &error)) {
        g_warning("Failed to add sRGB profile: %s", error->message);
        g_error_free(error); g_object_unref(profile); return NULL;
    }
    if (!cd_device_make_profile_default_sync(device, profile, NULL, &error)) {
        g_warning("Failed to make sRGB profile default: %s", error->message);
        g_error_free(error); g_object_unref(profile); return NULL;
    }
    return profile;
}
