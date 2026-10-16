// Copyright 2026 Alejandro Bermúdez Fajardo (alexoberco)
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;

namespace Sntv
{
    /// <summary>
    /// Receives an SNTV stream (https://github.com/alexoberco/sntv) and decodes it in hardware
    /// straight into <see cref="Texture"/>, without the frame ever passing through the CPU.
    ///
    /// Android only (Meta Quest and other Android XR devices), with the OpenGL ES 3 graphics API:
    /// the decoder's output is imported into GL as an external texture, which Vulkan does not do
    /// without more work. In the Editor and on other platforms it stays disabled and says so.
    ///
    /// The texture is created at the size the sender announces. Stereo streams carry both views
    /// side by side, left view in the left half. Each drawn frame's metadata (capture timestamp and,
    /// when the sender provides them, the pose each view was rendered from) is available through
    /// <see cref="TryGetFrame"/>, for reprojection or overlays.
    /// </summary>
    [DisallowMultipleComponent]
    public class SntvReceiver : MonoBehaviour
    {
        [Tooltip("Sender address: IPv4, IPv6 or a host name.")]
        public string host = "192.168.42.1";
        public int port = 8766;
        [Tooltip("Start receiving in Start(). Otherwise call StartReceiving().")]
        public bool autoStart = true;
        [Tooltip("Seconds between statistics lines in the log (0 = never).")]
        public float statsIntervalSeconds = 5f;

        /// <summary>Stream description, from the sender's header.</summary>
        public struct StreamInfo
        {
            public int version, width, height;
            public Codec codec;
            public float fps;
            public bool stereo;
            public Projection projection;
            public float fovDeg, lensC2;
            public bool hasViewPoses;
            public int Views => stereo ? 2 : 1;
        }

        public enum Codec { H264 = 1, HEVC = 2 }
        public enum Projection { Perspective = 0, FisheyeEquidistant = 1, FisheyeSine = 2 }

        /// <summary>Metadata of the frame currently in <see cref="Texture"/>.</summary>
        public struct FrameInfo
        {
            /// <summary>Capture time from the sender (simulation time for Gazebo), ns.</summary>
            public long timestampNs;
            /// <summary>Pose of each view's camera, converted to Unity axes (right, up, forward).
            /// Empty if the sender does not send poses.</summary>
            public Pose[] viewPoses;
        }

        /// <summary>The decoded video. Null until the sender's header arrives.</summary>
        public RenderTexture Texture => m_Texture;
        public bool IsConnected { get; private set; }
        /// <summary>Connected and at least one frame drawn into <see cref="Texture"/>.</summary>
        public bool HasVideo { get; private set; }
        public StreamInfo Info { get; private set; }

        /// <summary>Raised when a connection delivers its header (and on every reconnection).</summary>
        public event Action<StreamInfo> StreamStarted;
        /// <summary>Raised once per connection, when the first frame is in the texture.</summary>
        public event Action<RenderTexture> FirstFrame;

        public static bool Supported =>
            Application.platform == RuntimePlatform.Android &&
            SystemInfo.graphicsDeviceType == GraphicsDeviceType.OpenGLES3;

        // Mirrors of the C structs in Native~/sntv_android.c: same field order and types.
        [StructLayout(LayoutKind.Sequential)]
        struct NativeStats
        {
            public int connected, decoderRunning;
            public int version, width, height, codec, layout, projection, flags;
            public float fps, fovDeg, lensC2;
            public long framesReceived, framesDecoded, framesDrawn, bytesReceived;
            public float decodeMsAvg, decodeMsMax, drawMsAvg;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct NativeFrame
        {
            public long timestampNs;
            public int nPoses;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 14)]
            public float[] poses;
        }

#if UNITY_ANDROID && !UNITY_EDITOR
        const string k_Lib = "sntv";
        [DllImport(k_Lib)] static extern void sntv_start(string host, int port);
        [DllImport(k_Lib)] static extern void sntv_stop();
        [DllImport(k_Lib)] static extern void sntv_set_target(int glTexture, int width, int height);
        [DllImport(k_Lib)] static extern void sntv_get_stats(out NativeStats stats);
        [DllImport(k_Lib)] static extern void sntv_get_drawn_frame(out NativeFrame frame);
        [DllImport(k_Lib)] static extern IntPtr sntv_get_render_event_func();
#else
        static void sntv_start(string host, int port) { }
        static void sntv_stop() { }
        static void sntv_set_target(int glTexture, int width, int height) { }
        static void sntv_get_stats(out NativeStats stats) { stats = default; }
        static void sntv_get_drawn_frame(out NativeFrame frame) { frame = default; }
        static IntPtr sntv_get_render_event_func() => IntPtr.Zero;
#endif

        // The native receiver is a single instance per process.
        static SntvReceiver s_Active;

        RenderTexture m_Texture;
        IntPtr m_RenderFunc;
        bool m_Running;
        float m_NextStats;
        long m_LastDrawn, m_LastDecoded, m_LastReceived, m_LastBytes;   // for the statistics line
        long m_DrawnAtConnect;

        void Start()
        {
            if (autoStart) StartReceiving();
        }

        public void StartReceiving()
        {
            if (m_Running) return;
            if (!Supported)
            {
                Debug.LogWarning($"[SNTV] Needs Android with OpenGL ES 3 (now {Application.platform}, " +
                                 $"{SystemInfo.graphicsDeviceType}): receiver disabled.");
                return;
            }
            if (s_Active != null && s_Active != this)
            {
                Debug.LogError("[SNTV] Only one SntvReceiver can run at a time.");
                return;
            }
            s_Active = this;
            m_RenderFunc = sntv_get_render_event_func();
            sntv_start(host, port);
            m_Running = true;
            m_NextStats = Time.realtimeSinceStartup + statsIntervalSeconds;
            Debug.Log($"[SNTV] Waiting for video from {host}:{port}.");
        }

        public void StopReceiving()
        {
            if (!m_Running) return;
            sntv_stop();
            m_Running = false;
            IsConnected = HasVideo = false;
            if (s_Active == this) s_Active = null;
        }

        /// <summary>Metadata of the frame now in <see cref="Texture"/>. False before the first frame.</summary>
        public bool TryGetFrame(out FrameInfo frame)
        {
            frame = default;
            if (!HasVideo) return false;
            sntv_get_drawn_frame(out var f);
            frame.timestampNs = f.timestampNs;
            int n = f.poses == null ? 0 : Mathf.Clamp(f.nPoses, 0, 2);
            frame.viewPoses = new Pose[n];
            for (int v = 0; v < n; v++)
            {
                int o = v * 7;
                frame.viewPoses[v] = FromSenderFrame(
                    new Vector3(f.poses[o], f.poses[o + 1], f.poses[o + 2]),
                    new Quaternion(f.poses[o + 4], f.poses[o + 5], f.poses[o + 6], f.poses[o + 3]));
            }
            return true;
        }

        /// <summary>
        /// Sender axes (X forward, Y left, Z up, right-handed: Gazebo and ROS) to Unity's (X right,
        /// Y up, Z forward, left-handed). A camera with the returned rotation looks along its +Z
        /// where the sender's looked along its +X. Same as ROS-TCP-Connector's FLU.ConvertToRUF.
        /// </summary>
        public static Pose FromSenderFrame(Vector3 p, Quaternion q) =>
            new Pose(new Vector3(-p.y, p.z, p.x), new Quaternion(-q.y, q.z, q.x, -q.w));

        void Update()
        {
            if (!m_Running) return;
            sntv_get_stats(out var s);

            bool connected = s.connected != 0;
            if (connected && !IsConnected)
            {
                Info = new StreamInfo
                {
                    version = s.version, width = s.width, height = s.height, codec = (Codec)s.codec, fps = s.fps,
                    stereo = s.layout == 1, projection = (Projection)s.projection, fovDeg = s.fovDeg,
                    lensC2 = s.lensC2, hasViewPoses = (s.flags & 1) != 0,
                };
                HasVideo = false;
                m_DrawnAtConnect = s.framesDrawn;
                // The texture is born at the size the sender announces.
                if (m_Texture == null || m_Texture.width != s.width || m_Texture.height != s.height)
                    CreateTexture(s.width, s.height);
                Debug.Log($"[SNTV] Stream v{s.version}: {s.width}x{s.height} {(Codec)s.codec}, {s.fps:F0} fps, " +
                          $"{(s.layout == 1 ? "stereo" : "mono")}, {(Projection)s.projection} {s.fovDeg:F0} deg.");
                StreamStarted?.Invoke(Info);
            }
            IsConnected = connected;
            if (!connected) HasVideo = false;

            // The native draw runs on the render thread, in order with this frame's work: whatever
            // samples the texture afterwards in this frame sees the new image.
            if (m_Texture != null && m_RenderFunc != IntPtr.Zero) GL.IssuePluginEvent(m_RenderFunc, 1);

            if (connected && !HasVideo && s.framesDrawn > m_DrawnAtConnect && m_Texture != null)
            {
                HasVideo = true;
                FirstFrame?.Invoke(m_Texture);
            }

            if (statsIntervalSeconds > 0f && Time.realtimeSinceStartup >= m_NextStats)
            {
                float dt = statsIntervalSeconds;
                Debug.Log($"[SNTV] {(connected ? "connected" : "NOT CONNECTED")} | " +
                          $"received {(s.framesReceived - m_LastReceived) / dt:F1} fps, " +
                          $"{(s.bytesReceived - m_LastBytes) * 8 / dt / 1e6:F1} Mbit/s | " +
                          $"decoded {(s.framesDecoded - m_LastDecoded) / dt:F1} fps " +
                          $"(decode {s.decodeMsAvg:F1} ms, max {s.decodeMsMax:F1}) | " +
                          $"drawn {(s.framesDrawn - m_LastDrawn) / dt:F1} fps (render CPU {s.drawMsAvg:F2} ms)");
                m_LastDrawn = s.framesDrawn;
                m_LastReceived = s.framesReceived;
                m_LastDecoded = s.framesDecoded;
                m_LastBytes = s.bytesReceived;
                m_NextStats = Time.realtimeSinceStartup + statsIntervalSeconds;
            }
        }

        void CreateTexture(int width, int height)
        {
            if (m_Texture != null) m_Texture.Release();
            // sRGB: the native draw decodes before GL re-encodes on write, so the stored values are
            // the video's and sampling returns linear colour, as Unity expects.
            m_Texture = new RenderTexture(width, height, GraphicsFormat.R8G8B8A8_SRGB, GraphicsFormat.None)
            {
                name = "SNTV",
                useMipMap = false,
                autoGenerateMips = false,
                filterMode = FilterMode.Bilinear,
                wrapMode = TextureWrapMode.Clamp,
            };
            m_Texture.Create();
            sntv_set_target((int)m_Texture.GetNativeTexturePtr(), width, height);
        }

        void OnDestroy()
        {
            StopReceiving();
            if (m_Texture != null) m_Texture.Release();
        }
    }
}
