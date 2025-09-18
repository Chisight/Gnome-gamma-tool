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
} AppArgs;

// --- Forward Declarations of Helper Functions ---
static void parse_arguments(int argc, char *argv[], AppArgs *args);
static GList *get_display_devices(CdClient *client);
static void handle_info_mode(CdDevice *device, CdProfile *profile);
static void handle_remove_mode(CdDevice *device, CdProfile *profile);
static void handle_apply_mode(CdClient *client, CdDevice *device, CdProfile *profile, AppArgs *args);
static void generate_vcgt(gfloat gamma[3], gint color_temperature, CdIcc *profile_data);
static CdProfile *create_and_set_sRGB_profile(CdClient *client, CdDevice *device);


/**
 * @brief The main entry point of the gamma-tool program.
 *
 * Orchestrates the entire process: parses arguments, connects to the colord service,
 * discovers all display devices, and then iterates through them, delegating the
 * main task (info, remove, or apply) to the appropriate handler function.
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
    // Create a new client object.
    CdClient *client = cd_client_new();
    // Synchronously connect the client to the running colord service.
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
    
    // --- Main Loop: Process Each Display ---
    for (GList *l = display_devices; l != NULL; l = l->next) {
        CdDevice *device = l->data;
        printf("\ndevice: %s\n", cd_device_get_id(device));

        // Get the list of profiles associated with this device.
        GPtrArray *profiles = cd_device_get_profiles(device);
        CdProfile *base_profile = NULL;

        // The first profile in the list is the current default.
        if (profiles != NULL && profiles->len > 0) {
            base_profile = g_object_ref(g_ptr_array_index(profiles, 0));
        } else {
            // If there's no profile, the device might not be managed.
            // We create and set sRGB as a sensible default.
            printf("No default profile, using sRGB\n");
            base_profile = create_and_set_sRGB_profile(client, device);
            if (!base_profile) {
                g_warning("Could not set sRGB profile for %s. Skipping.", cd_device_get_id(device));
                if (profiles) g_ptr_array_free(profiles, TRUE);
                continue;
            }
        }
        if (profiles) g_ptr_array_free(profiles, TRUE);
        
        // As with devices, profile objects are proxies and must be connected before use.
        if (!cd_profile_connect_sync(base_profile, NULL, &error)) {
            g_warning("Could not connect to base profile: %s", error->message);
            g_error_free(error); error = NULL;
            g_object_unref(base_profile);
            continue;
        }

        // --- Delegate to appropriate handler based on mode ---
        if (args.info_mode) {
            handle_info_mode(device, base_profile);
        } else if (args.remove_profile) {
            handle_remove_mode(device, base_profile);
        } else {
            handle_apply_mode(client, device, base_profile, &args);
        }
        
        // Clean up the reference to the profile for this device.
        g_object_unref(base_profile);
    }

    // --- Final Cleanup ---
    // Free the list of devices and unreference the client itself.
    g_list_free_full(display_devices, g_object_unref);
    g_object_unref(client);

    return 0;
}

/**
 * @brief Parses command-line arguments and populates the AppArgs struct.
 *
 * Sets default values, then iterates through argv to find flags like -g, -t, -r, -i.
 * It handles different forms like "-g 0.8" and "-g=0.8". Exits with usage info
 * if arguments are invalid or insufficient.
 *
 * @param argc   (Input) The argument count from main.
 * @param argv   (Input) The argument vector from main.
 * @param args   (Output) Pointer to the struct to be filled with parsed values.
 */
static void parse_arguments(int argc, char *argv[], AppArgs *args) {
    // Set default values.
    *args = (AppArgs){
        .gamma = {1.0f, 1.0f, 1.0f},
        .temperature = 6500,
        .remove_profile = FALSE,
        .info_mode = FALSE,
    };
    const char *gamma_str = "1.0";

    for (int i = 1; i < argc; ++i) {
        if (g_strcmp0(argv[i], "-r") == 0) {
            args->remove_profile = TRUE;
        } else if (g_strcmp0(argv[i], "-i") == 0) {
            args->info_mode = TRUE;
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

    // GLib's g_strsplit is a convenient way to handle the "R:G:B" format.
    gchar **parts = g_strsplit(gamma_str, ":", 3);
    if (parts[0] && !parts[1]) {
        gfloat val = g_ascii_strtod(parts[0], NULL);
        args->gamma[0] = val; args->gamma[1] = val; args->gamma[2] = val;
    } else if (parts[0] && parts[1] && parts[2]) {
        args->gamma[0] = g_ascii_strtod(parts[0], NULL);
        args->gamma[1] = g_ascii_strtod(parts[1], NULL);
        args->gamma[2] = g_ascii_strtod(parts[2], NULL);
    }
    g_strfreev(parts); // Clean up the memory allocated by g_strsplit.

    if (argc < 2) {
        fprintf(stderr, "Usage: %s [-g R:G:B|G] [-t TEMP] [-r] [-i]\n", argv[0]);
        fprintf(stderr, "  -g GAMMA       Target gamma (e.g., 0.8), 1.0 is neutral.\n");
        fprintf(stderr, "  -t TEMPERATURE Target color temperature, 6500 is neutral.\n");
        fprintf(stderr, "  -r             Remove existing profile created by this tool.\n");
        fprintf(stderr, "  -i             Display info about the current profile.\n");
        exit(1);
    }
}

/**
 * @brief Gets a list of all connected display devices from the colord service.
 *
 * Fetches all devices known to colord, then iterates through them. It's crucial
 * to connect to each device proxy before querying its properties (like its kind).
 * It filters for devices of kind DISPLAY and returns them in a new list.
 *
 * @param client (Input) The connected CdClient instance.
 * @return A GList of connected CdDevice objects. The caller must free this list
 *         and its contents using g_list_free_full(list, g_object_unref).
 */
static GList *get_display_devices(CdClient *client) {
    GError *error = NULL;
    // Get all devices known to the colord service.
    GPtrArray *all_devices = cd_client_get_devices_sync(client, NULL, &error);
    if (error) {
        g_critical("Failed to get devices: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    GList *display_devices = NULL;
    for (guint i = 0; i < all_devices->len; i++) {
        CdDevice *device = g_ptr_array_index(all_devices, i);

        // CRITICAL: The returned devices are lightweight proxies. We must explicitly
        // connect to them before we can query their properties.
        if (!cd_device_connect_sync(device, NULL, &error)) {
            g_warning("Could not connect to device %s: %s", cd_device_get_id(device), error->message);
            g_error_free(error);
            error = NULL;
            continue;
        }

        // Filter for display devices only.
        if (cd_device_get_kind(device) == CD_DEVICE_KIND_DISPLAY) {
            // Add a reference to the device for our new list.
            display_devices = g_list_append(display_devices, g_object_ref(device));
        }
    }
    // Free the original array of proxies.
    g_ptr_array_free(all_devices, TRUE);
    return display_devices;
}

/**
 * @brief Handles the -i (info) mode for a single device.
 *
 * Checks if the current profile's filename matches the tool's prefix. If so, it
 * uses sscanf to parse the gamma and temperature values from the filename and prints them.
 *
 * @param device  (Input) The current display device (for context).
 * @param profile (Input) The currently active profile for the device.
 */
static void handle_info_mode(CdDevice *device, CdProfile *profile) {
    const char *profile_filename = cd_profile_get_filename(profile);
    if (profile_filename == NULL) {
        printf("Current profile has no filename.\n");
        return;
    }
    
    // Get just the filename part of the path to check against our prefix.
    gchar *basename = g_path_get_basename(profile_filename);
    if (g_str_has_prefix(basename, OUR_PREFIX)) {
        int r, g, b, temp;
        // sscanf is a simple way to parse formatted strings.
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
 *
 * Checks if the active profile was created by this tool. If so, it disassociates
 * the profile from the device, which causes colord to automatically fall back to
 * the next best profile. Finally, it deletes the physical .icc file.
 *
 * @param device  (Input) The display device to remove the profile from.
 * @param profile (Input) The profile to potentially remove.
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
        // This tells colord to disassociate the profile from the device.
        // The system will then automatically activate the next-highest priority profile.
        if (cd_device_remove_profile_sync(device, profile, NULL, &error)) {
            printf("Deleting file %s\n", profile_filename);
            // This deletes the actual file from the disk.
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
 *
 * This is the core "write" logic. It loads ICC data from a base profile,
 * modifies it with a new VCGT, saves it to a unique file, polls for colord to
 * detect the file, and then makes it the default for the device. It also
 * cleans up any previous profile created by this tool.
 *
 * @param client  (Input) The connected CdClient, needed to poll for the new profile.
 * @param device  (Input) The display device to apply the new profile to.
 * @param profile (Input) The base profile to clone data from.
 * @param args    (Input) The parsed application arguments with desired gamma/temp.
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
    // Load the raw ICC data from the current profile into a modifiable object.
    CdIcc *profile_data = cd_profile_load_icc(profile, CD_ICC_LOAD_FLAGS_NONE, NULL, &error);
    if (error) {
        g_warning("Could not get ICC data from base profile: %s", error->message);
        g_error_free(error);
        return;
    }
    
    gchar *title = g_strdup_printf("gamma-tool: g=%.2f:%.2f:%.2f t=%d", args->gamma[0], args->gamma[1], args->gamma[2], args->temperature);
    cd_icc_set_description(profile_data, "", title);
    g_free(title);

    // Add a unique UUID to ensure the file contents are unique, even with identical settings.
    gchar *uuid_str = g_uuid_string_random();
    cd_icc_add_metadata(profile_data, "uuid", uuid_str);
    // Calculate and apply the new gamma table to the ICC data.
    generate_vcgt(args->gamma, args->temperature, profile_data);

    // Construct the new filename with embedded parameters. e.g., g080075100t5500
    int r = (int)(args->gamma[0] * 100.0f);
    int g = (int)(args->gamma[1] * 100.0f);
    int b = (int)(args->gamma[2] * 100.0f);
    gchar *new_basename = g_strdup_printf("%sg%03d%03d%03dt%d-%s.icc",
                                          OUR_PREFIX, r, g, b, args->temperature, uuid_str);
    
    // Build the full path to the user's local ICC directory.
    gchar *icc_dir = g_build_filename(g_get_user_data_dir(), "icc", NULL);
    g_mkdir_with_parents(icc_dir, 0755); // Ensure directory exists.
    gchar *new_path = g_build_filename(icc_dir, new_basename, NULL);
    // GFile is a modern, abstract way to handle file paths provided by GIO.
    GFile *profile_file = g_file_new_for_path(new_path);
    CdProfile *new_profile = NULL;
    
    // Save the modified ICC data to the new file.
    if (!cd_icc_save_file(profile_data, profile_file, CD_ICC_SAVE_FLAGS_NONE, NULL, &error)) {
        g_warning("Could not save new profile to %s: %s", new_path, error->message);
        g_error_free(error);
    } else {
        // Poll for a few seconds waiting for the colord service to detect the new file.
        gint64 deadline = g_get_monotonic_time() + TIMEOUT_SECONDS * G_TIME_SPAN_SECOND;
        while (g_get_monotonic_time() < deadline) {
            new_profile = cd_client_find_profile_by_filename_sync(client, new_path, NULL, NULL);
            if (new_profile) break;
            // This allows the main event loop (and thus colord's file monitor) to run.
            g_main_context_iteration(NULL, FALSE);
            g_usleep(10000); // Sleep 10ms to avoid busy-waiting.
        }
        
        if (new_profile && cd_profile_connect_sync(new_profile, NULL, &error)) {
            printf("New profile is %s\n", cd_profile_get_filename(new_profile));
            // Associate the new profile with the device.
            if (!cd_device_add_profile_sync(device, CD_DEVICE_RELATION_HARD, new_profile, NULL, NULL))
                g_warning("Failed to add new profile to device.");
            // Make the new profile the active one.
            if (!cd_device_make_profile_default_sync(device, new_profile, NULL, NULL))
                g_warning("Failed to make new profile default.");
        } else if (new_profile) { // Connection to the new profile failed.
            g_warning("Could not connect to new profile: %s", error->message);
            g_error_free(error);
        } else { // Loop timed out.
            g_warning("Timed out waiting for colord to detect new profile: %s", new_path);
        }
    }

    // If we successfully created a new profile and the old one was ours, remove the old one.
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

    // Final cleanup for this iteration.
    if (new_profile) g_object_unref(new_profile);
    g_object_unref(profile_file);
    g_free(new_basename);
    g_free(icc_dir);
    g_free(new_path);
    g_free(uuid_str);
    g_object_unref(profile_data);
}


/**
 * @brief Generates a Video Card Gamma Table (VCGT) and applies it to an ICC profile.
 *
 * Translates the user-specified gamma and temperature into a 256-entry gamma table.
 * Each entry is a CdColorRGB struct allocated on the heap, and pointers to them are
 * stored in a GPtrArray as required by the cd_icc_set_vcgt function.
 *
 * @param gamma             (Input) Array of three floats for R, G, B gamma.
 * @param color_temperature (Input) The target color temperature in Kelvin.
 * @param profile_data      (Input/Output) The CdIcc object to modify.
 */
static void generate_vcgt(gfloat gamma[3], gint color_temperature, CdIcc *profile_data) {
    gfloat gamma_factor[3];
    gamma_factor[0] = 1.0f / gamma[0];
    gamma_factor[1] = 1.0f / gamma[1];
    gamma_factor[2] = 1.0f / gamma[2];
    CdColorRGB temp_color;
    GError *error = NULL;
    // We need an array of *pointers* to structs, and g_ptr_array_new_with_free_func
    // ensures the structs themselves are freed when the array is.
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
 *
 * This is used as a fallback when a display has no default profile to begin with.
 *
 * @param client (Input) The connected CdClient to search for the profile.
 * @param device (Input) The device to apply the sRGB profile to.
 * @return A new reference to the CdProfile object on success, NULL on failure.
 *         The caller is responsible for unreferencing the returned object.
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
