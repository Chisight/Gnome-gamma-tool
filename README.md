# gamma-tool

A simple, Wayland compatible, lightweight command-line tool for Linux to adjust display gamma and color temperature. It interacts with the `colord` service to manage and apply color profiles on a per-monitor basis.

## Features

-   **Gamma Correction**: Apply a uniform gamma correction across all channels (e.g., `0.8`) or specify individual values for Red, Green, and Blue (e.g., `0.9:0.8:0.7`).
-   **Color Temperature**: Adjust the color temperature of your display (e.g., `5000` for a warmer, "night light" effect. Standard temperature is 6500).
-   **Multi-Monitor Support**: Automatically detects and applies settings to all connected display devices.
-   **Profile Management**:
    -   Creates new, uniquely named `.icc` profiles with the settings embedded in the filename.
    -   Cleans up old profiles created by this tool when applying a new one.
    -   Can remove its own profiles, safely reverting to the system's default.
-   **Inspect Settings**: Read the gamma and temperature settings from a profile filename created by this tool.
-   **Wayland Gnome and Mutter compatibility**: Tested with Gnome 43.

## Requirements

To compile this tool, you will need `gcc`, `pkg-config`, and the development headers for GLib and Colord.

On Debian, Ubuntu, or derivative systems, you can install these with:

```bash
sudo apt install build-essential pkg-config libglib2.0-dev libcolord-dev
```

## Compilation

The project consists of a single C file.

You can git clone, download the .zip, or simply curl the file:

```bash
curl -o gamma-tool.c https://raw.githubusercontent.com/Chisight/Gnome-gamma-tool/refs/heads/main/gamma-tool.c
```

You can compile it using the following command. The command uses `pkg-config` to automatically find the necessary compiler and linker flags for the required libraries:

```bash
gcc -o gamma-tool gamma-tool.c $(pkg-config --cflags --libs glib-2.0 gobject-2.0 colord gio-2.0) -lm
```
This will create an executable file named `gamma-tool` in the current directory.

## Usage

The tool operates on all monitors at once and has three primary modes: applying settings, removing settings, or inspecting settings.

```
Usage: ./gamma-tool [-g R:G:B|G] [-t TEMP] [-r] [-i]
```

### Options

| Flag | Argument        | Description                                                                                              |
| :--- | :-------------- | :------------------------------------------------------------------------------------------------------- |
| `-g` | `GAMMA`         | Sets the target gamma. Can be a single float (e.g., `0.9`) or three colon-separated floats for R:G:B (e.g., `1.0:0.95:0.9`). `1.0` is neutral. |
| `-t` | `TEMPERATURE`   | Sets the target color temperature in Kelvin. `6500` is neutral (daylight).                               |
| `-r` | _(none)_        | **Remove mode**: Finds the active profile created by this tool, removes it, and reverts to the system default. |
| `-i` | _(none)_        | **Info mode**: Inspects the active profile and, if created by this tool, prints the settings parsed from its filename. |
| `-d` | `device`        | **Single Display mode**: Applies changes only to given device number, zero based. |

### Examples

#### 1. Set a Warmer Color Temperature (Night Light)

This is useful for reducing blue light in the evening.

```bash
./gamma-tool -t 5000
```

#### 2. Apply a Uniform Gamma Correction

Make the entire screen slightly darker.

```bash
./gamma-tool -g 1.1
```

#### 3. Apply a Custom Per-Channel Gamma Correction

Reduce the blue channel gamma to give the screen a yellowish tint.

```bash
./gamma-tool -g 1.0:1.0:0.7
```

#### 4. Check the Current Settings

If the active profile was created by this tool, this command will display its settings.

```bash
./gamma-tool -i
```
**Example Output:**
```
device: xrandr-Dell-U2724DE-0x000008ab
gamma: 1.00:1.00:0.70
temperature: 6500
```

#### 5. Remove the Custom Profile and Revert

This will remove the active `gamma-tool` profile and revert each monitor to its default (usually the EDID or sRGB profile).

```bash
./gamma-tool -r
```

## How It Works

This tool does not create color profiles from scratch. Instead, it performs the following steps:

1.  Connects to the system's `colord` daemon.
2.  For each display, it finds the currently active color profile (e.g., the default profile derived from the monitor's EDID).
3.  It loads this base profile into memory and modifies it by adding a **VCGT (Video Card Gamma Table)** tag. This tag contains the calculated gamma and temperature curves.
4.  It saves this modified data to a new `.icc` file in `~/.local/share/icc/` with a unique, descriptive filename.
5.  It instructs `colord` to make this new profile the default for the display.
6.  If the previously active profile was also created by `gamma-tool`, it is removed to prevent clutter.

The `-r` option simply tells `colord` to disassociate the custom profile, which causes the system to automatically fall back to the next-best default.

## License

This project is licensed under the GPL-3.0 license
