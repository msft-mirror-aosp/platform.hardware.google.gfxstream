# Graphics Streaming Kit (formerly: Vulkan Cereal)

Graphics Streaming Kit (colloquially known as Gfxstream) is a code generator
that makes it easier to serialize and forward graphics API calls from one place
to another:

-   From a virtual machine guest to host for virtualized graphics
-   From one process to another for IPC graphics
-   From one computer to another via network sockets

# Build: Linux

The latest directions for the standalone Linux build are provided
[here](https://crosvm.dev/book/appendix/rutabaga_gfx.html).

# Build: Windows

Make sure the latest CMake is installed. Make sure Visual Studio 2019 is
installed on your system along with all the Clang C++ toolchain components.
Then:

```
mkdir build
cd build
cmake . ../ -A x64 -T ClangCL
```

A solution file should be generated. Then open the solution file in Visual
studio and build the `gfxstream_backend` target.

# Build: Android for host

Be in the Android build system. Then:

```
m libgfxstream_backend
```

It then ends up in `out/host`

This also builds for Android on-device.

# Output artifacts

```
libgfxstream_backend.(dll|so|dylib)
```

# Regenerating Vulkan code

To re-generate both guest and Vulkan code, please run:

scripts/generate-gfxstream-vulkan.sh

# Regenerating GLES/RenderControl code

First, build `build/gfxstream-generic-apigen`. Then run:

```
scripts/generate-apigen-source.sh
```

# Tests

## Windows Tests

There are a bunch of test executables generated. They require `libEGL.dll` and
`libGLESv2.dll` and `vulkan-1.dll` to be available, possibly from your GPU
vendor or ANGLE, in the `%PATH%`.

## Android Host Tests

There are Android mock testa available, runnable on Linux. To build these tests,
run:

```
m GfxstreamEnd2EndTests
```

# Features

## Tracing

The host renderer has optional support for Perfetto tracing which can be enabled
by defining `GFXSTREAM_BUILD_WITH_TRACING` (enabled by default on Android
builds).

The `perfetto` and `traced` tools from Perfetto should be installed. Please see
the [Perfetto Quickstart](https://perfetto.dev/docs/quickstart/linux-tracing) or
follow these short form instructions:

```
cd <your Android repo>/external/perfetto

./tools/install-build-deps

./tools/gn gen --args='is_debug=false' out/linux

./tools/ninja -C out/linux traced perfetto
```

To capture a trace on Linux, start the Perfetto daemon:

```
./out/linux/traced
```

Then, run Gfxstream with
[Cuttlefish](https://source.android.com/docs/devices/cuttlefish):

```
cvd start --gpu_mode=gfxstream_guest_angle_host_swiftshader
```

Next, start a trace capture with:

```
./out/linux/perfetto --txt -c gfxstream_trace.cfg -o gfxstream_trace.perfetto
```

with `gfxstream_trace.cfg` containing the following or similar:

```
buffers {
  size_kb: 4096
}
data_sources {
  config {
    name: "track_event"
    track_event_config {
    }
  }
}
```

Next, end the trace capture with Ctrl + C.

Finally, open https://ui.perfetto.dev/ in your webbrowser and use "Open trace
file" to view the trace.

# Design Notes

## Guest Vulkan

gfxstream vulkan is the most actively developed component. Some key commponents
of the current design include:

-   1:1 threading model - each guest Vulkan encoder thread gets host side
    decoding thread
-   Support for both virtio-gpu, goldish and testing transports.
-   Support for Android, Fuchsia, and Linux guests.
-   Ring Buffer to stream commands, in the style of io_uring.
-   Mesa embedded to provide
    [dispatch](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/docs/vulkan/dispatch.rst)
    and
    [objects](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/docs/vulkan/base-objs.rst).
-   Currently, there are a set of Mesa objects and gfxstream objects. For
    example, `struct gfxstream_vk_device` and the gfxstream object
    `goldfish_device` both are internal representations of Vulkan opaque handle
    `VkDevice`. The Mesa object is used first, since Mesa provides dispatch. The
    Mesa object contains a key to the hash table to get a gfxstream internal
    object (for example, `gfxstream_vk_device::internal_object`). Eventually,
    gfxstream objects will be phased out and Mesa objects used exclusively.
