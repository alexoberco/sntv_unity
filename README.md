# sntv_unity — SNTV receiver for Unity

A Unity package that receives [SNTV](https://github.com/alexoberco/sntv) video
on **Android / Meta Quest** and decodes it in hardware **straight into a
RenderTexture**: the frame goes from the socket to the hardware decoder to a GPU
buffer to your texture, without a single CPU copy.

Built for [Sentinel](https://github.com/alexoberco), a VR teleoperation project,
to show a simulated robot's stereo fisheye eyes on a Quest 3S. Pair it with
[sntv_gazebo](https://github.com/alexoberco/sntv_gazebo) (Gazebo + NVENC) or
anything else that speaks SNTV.

## Why

Unity's WebRTC package decodes in hardware on Android but uploads every frame
to its texture **through the CPU on the render thread**: ~9 ms per frame at
2×1280 px, measured with the Unity Profiler on a Quest 3S, which rules out high
resolutions. This receiver imports the decoder's output buffer into OpenGL and
draws it into your texture in one pass. Measured on a Quest 3S with a
4320×2176 HEVC stream, with the Sentinel receiver this package was extracted
from: **10.2 ms to decode** (on the video chip, off the CPU) and **0.41 ms of
render thread** per frame, at 44.6 fps. This 1.0.0 package itself has been
compiled and checked against the protocol, not yet run on a headset.

## Requirements

- Unity 6 (6000.0 or later), Android build target, **arm64**.
- Graphics API **OpenGL ES 3** (Player Settings → Other Settings → untick *Auto
  Graphics API*, put OpenGLES3 first). The decoder's buffers are imported as
  external GL textures; Vulkan is not supported.
- An SNTV sender reachable from the headset. Over a USB cable, the Quest can
  expose a real network interface (USB NCM); see the
  [Sentinel host setup](https://github.com/alexoberco/docker_image_dev_container_ros2/tree/sentinel/host)
  for making it come up by itself on Linux.

## Install

Package Manager → **+** → *Install package from git URL*:

```
https://github.com/alexoberco/sntv_unity.git
```

or in `Packages/manifest.json`: `"com.alexoberco.sntv": "https://github.com/alexoberco/sntv_unity.git#v1.0.0"`.

## Use

1. Add **SntvReceiver** to a GameObject; set `host` and `port` (default
   `192.168.42.1:8766`).
2. To just look at it, add a Quad with **SntvDisplay**: mono streams as they
   are, stereo side-by-side streams with each half to its eye.
3. Or use the texture yourself:

```csharp
var rx = GetComponent<Sntv.SntvReceiver>();
rx.FirstFrame += tex => myMaterial.mainTexture = tex;

// Every frame: the pose each view was rendered from, in Unity axes.
if (rx.TryGetFrame(out var f) && f.viewPoses.Length > 0)
    Debug.Log($"captured at {f.timestampNs} ns from {f.viewPoses[0].position}");
```

`SntvReceiver.Info` has the stream's size, codec, frame rate, layout and lens
(projection, field of view, sine `c2`), so you can build your own display: for
immersive fisheye video, map the projection from the
[spec](https://github.com/alexoberco/sntv/blob/main/SPEC.md#projections) onto
a sphere around the head. The per-frame view poses let you reproject the image
against the current head pose and hide the round-trip latency.

Poses arrive in the sender's frame (X forward, Y left, Z up — Gazebo/ROS) and
`TryGetFrame` converts them to Unity's (X right, Y up, Z forward), the same
conversion as ROS-TCP-Connector's `FLU.ConvertToRUF`.

### Tips for a smooth headset image

- Pick a display refresh that is a whole multiple of the stream's fps (`Info.fps`):
  45 fps → 90 Hz, 60 fps → 120 Hz. At 60 fps on 90 Hz every frame lasts 1.5
  refreshes and the motion judders.
- One receiver per app: the native side is a single instance.

## In the log

```bash
adb logcat -s Unity:* SNTV:*
```

Every 5 s: `[SNTV] connected | received 44.6 fps, 62.6 Mbit/s | decoded 44.8 fps (decode 10.2 ms, max 14.1) | drawn 44.6 fps (render CPU 0.41 ms)`.

## Building the native library

`Runtime/Plugins/Android/arm64-v8a/libsntv.so` is prebuilt. To rebuild it from
`Native~/sntv_android.c` with the NDK that ships with the Unity Editor:

```bash
Native~/build.sh          # UNITY=<editor dir> or NDK=<ndk dir> to choose
```

`Native~/sntv/sntv.h` is a copy of the one in the
[sntv](https://github.com/alexoberco/sntv) repo.

## License and credit

[Apache License 2.0](LICENSE). Use it, change it, sell products built on it. In
exchange, any redistribution must keep the copyright notices and ship the
[NOTICE](NOTICE) file, or its text in your credits or documentation.
