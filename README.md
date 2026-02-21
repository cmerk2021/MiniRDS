# MiniRDS

### This is the world's first open-source RDS2 encoder!

This program is designed for generating a realtime RDS signal. It is capable of RDS2 using up to 3 additional subcarriers.

This is based on the RDS encoder from [Mpxgen](https://github.com/Anthony96922/mpxgen), which is currently not maintained.

![MiniRDS on Eton/Tecsun](doc/mpxgen.jpg)

This software is currently used as the RDS encoder for KPSK in Los Angeles, CA, USA.

#### Features
- Low resource requirements
- Support for basic RDS data fields: PS, RT, PTY and AF
- RDS items can be updated through control pipe
- RT+ support
- RDS2 support (including station logo transmission)

#### To do
- Threading

#### Planned features
- UECP
- Configuration file

RDS2 image reception in action: https://www.bitchute.com/video/sNXyTCCAYA8l/

## Build

### Linux (Debian/Ubuntu)

Install deps: `sudo apt-get install libao-dev libsamplerate0-dev`

Then build:
```sh
git clone https://github.com/cmerk2021/MiniRDS
cd MiniRDS/src
make
```

Alternatively, using CMake:
```sh
git clone https://github.com/Anthony96922/MiniRDS
cd MiniRDS
mkdir build && cd build
cmake ..
cmake --build .
```

### Windows

MiniRDS can be built on Windows using **MSYS2/MinGW-w64** or **CMake with vcpkg**.

#### Option A: MSYS2 / MinGW-w64 (Recommended)

1. **Install MSYS2** from https://www.msys2.org/

2. **Open the MSYS2 UCRT64 terminal** and install dependencies:
   ```sh
   pacman -Syu
   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-make
   pacman -S mingw-w64-ucrt-x86_64-libao mingw-w64-ucrt-x86_64-libsamplerate
   ```

3. **Clone and build**:
   ```sh
   git clone https://github.com/cmerk2021/MiniRDS
   cd MiniRDS
   mkdir build && cd build
   cmake -G "MinGW Makefiles" ..
   cmake --build .
   ```

4. The `minirds.exe` (CLI) and `minirds_gui.exe` (GUI) executables will be in the `build/` directory.

   To build only the CLI without the GUI:
   ```sh
   cmake -G "MinGW Makefiles" -DBUILD_GUI=OFF ..
   ```

5. **To run**, make sure the MinGW DLLs are accessible. Either:
   - Run from the MSYS2 UCRT64 terminal, or
   - Copy the required DLLs (`libao-4.dll`, `libsamplerate-0.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, `libstdc++-6.dll`) from `C:\msys64\ucrt64\bin\` into the same folder as `minirds.exe`.

#### Option B: CMake + vcpkg (Visual Studio)

1. **Install prerequisites**:
   - [Visual Studio 2022](https://visualstudio.microsoft.com/) with "Desktop development with C++" workload
   - [CMake](https://cmake.org/download/) (3.14 or later)
   - [vcpkg](https://github.com/microsoft/vcpkg):
     ```powershell
     git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
     C:\vcpkg\bootstrap-vcpkg.bat
     ```

2. **Install dependencies via vcpkg**:
   ```powershell
   C:\vcpkg\vcpkg install libao:x64-windows libsamplerate:x64-windows
   ```
   > Note: If `libao` is not available via vcpkg, use the MSYS2 method instead.

3. **Clone and build**:
   ```powershell
   git clone https://github.com/Anthony96922/MiniRDS
   cd MiniRDS
   mkdir build; cd build
   cmake -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake ..
   cmake --build . --config Release
   ```

4. The `minirds.exe` executable will be in `build\Release\`.

#### Windows Notes

- **Control pipe**: On Windows, the `--ctl` option creates a Windows Named Pipe instead of a UNIX FIFO. The pipe name is automatically prefixed with `\\.\pipe\` if you pass a simple name (e.g., `--ctl minirds` creates `\\.\pipe\minirds`). You can write commands to it using PowerShell:
  ```powershell
  # Example: change PS text
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "minirds", [System.IO.Pipes.PipeDirection]::Out)
  $pipe.Connect(1000)
  $writer = New-Object System.IO.StreamWriter($pipe)
  $writer.WriteLine("PS MyRadio")
  $writer.Flush()
  $writer.Close()
  ```

- **Network control**: The socket-based control (`--port`) works the same way on both platforms.

- **RDS2 station logo**: On Windows, the default station logo path is `rds2-image\stationlogo.png` relative to the working directory (instead of `/tmp/rds2-image/stationlogo.png` on Linux).

## How to use

### GUI Mode (Windows)

Run `minirds_gui.exe` for a graphical interface that provides:

- **RDS Settings**: Edit PI code, PS, RT, PTY, PTYN, AF frequencies, Long PS, and eRT. Toggle TP/TA/MS flags with checkboxes. Click "Apply Settings" to update parameters in real-time while the encoder is running.
- **Audio Output**: Select from available Windows audio devices (enumerated via WMM). Adjust output volume with the slider.
- **Control**: Start/Stop the RDS encoder. Load and execute command files (text files containing one ASCII command per line — same format as the control pipe).
- **Log**: Real-time log output showing encoder status, errors, and diagnostics.

**Command files** are plain text files with one command per line (lines starting with `#` are comments). See the [command list](doc/command_list.md) for valid commands. Example:
```
# Set station info
PS MyRadio
RT Now playing: Great Music
PTY 10
TP ON
AF 98.1
AF 101.3
```

### CLI Mode

Simply run:
```
./minirds
```
to confirm proper operation.

Please see `-h` for more options.

### Stereo Tool integration
The following setup allows MiniRDS to be used alongside Stereo Tool audio processor.
```
.-------------.
| Stereo Tool |--(FM MPX w/o RDS)-----.
'-------------'                       |
                                      v
                           .-----------------------.
                           |       ALSA dmixer     |
                           | slave: MPX sound card |--------(to sound card)-------->
                           |        192 kHz        |
                           '-----------------------'
                                      ^
.---------.                           |
| MiniRDS |--(RDS output)-------------'
'---------'
```

First, add the following contents to ~/.asoundrc:
```
# ST MPX output
pcm.mpxmix {
  type dmix
  ipc_key 1001
  slave {
    pcm "digital-out" # change to your actual sound card
    rate 192000
  }
}
```

Next, add the collowing contents to ~/.ao:
```
dev=mpxmix
```

Then set the Stereo Tool MPX output to use the ALSA "mpxmix" output. Finally run minirds. *Adjust volumes accordingly.*

Note that this setup is not optimal. Hans plans to add RDS2 passthough to the ST external RDS input. [Stereo Tool forum post](https://forums.stereotool.com/viewtopic.php?f=14&t=33793&start=150)

### Changing PS, RT, TA and PTY at run-time
You can control PS, RT, TA (Traffic Announcement flag), PTY (Program Type) and many other items at run-time using a named pipe (FIFO). For this run MiniRDS with the `--ctl` argument.

Scripts can be written to obtain and send "now playing" text data to MiniRDS for dynamic RDS.

See the [command list](doc/command_list.md) for a complete list of valid commands.

### RDS2
MiniRDS has a working implementation of the RFT protocol in RDS2. Please edit the Makefile accordingly and rebuild for RDS2 capabilities. You may use your own image by using the provided "make-station-logo.sh" script. Valid formats are PNG or JPG and should be about 3kB or less. Larger images take considerably longer to receive.

![RDS2 RFT](doc/rds2-rft.png)

## References
- [EN 50067, Specification of the radio data system (RDS) for VHF/FM sound broadcasting in the frequency range 87.5 to 108.0 MHz](http://www.interactive-radio-system.com/docs/EN50067_RDS_Standard.pdf)
- [IEC 62106-2, Radio data system (RDS) – Part 2: Message format: coding and definitions of RDS features](http://downloads.dxing.si/download.php?file=ISO%20Stamdards/RDS/latest%20(includes%20RDS2)/iec-62106-2-2021.pdf)
- [IEC 62106-3, Radio data system (RDS) – Part 3: Usage and registration of Open Data Applications (ODAs)](http://downloads.dxing.si/download.php?file=ISO%20Stamdards/RDS/latest%20(includes%20RDS2)/iec-62106-3-2018.pdf)
- [IEC 62106-4, Radio data system (RDS) – Part 4: Registered code tables](http://downloads.dxing.si/download.php?file=ISO%20Stamdards/RDS/latest%20(includes%20RDS2)/iec-62106-4-2018.pdf)
- [IEC 62106-6, Radio data system (RDS) – Part 6: Compilation of technical specifications for Open Data Applications in the
public domain](http://downloads.dxing.si/download.php?file=ISO%20Stamdards/RDS/latest%20(includes%20RDS2)/iec-62106-6-2018.pdf)
- [P232 RDS Encoder
Technical Manual](https://pira.cz/rds/p232man.pdf)

## Credits
The RDS waveform generator was adapted from [PiFmRds](https://github.com/ChristopheJacquet/PiFmRds)
