// Copyright 2026 Alejandro Bermúdez Fajardo (alexoberco)
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace Sntv
{
    /// <summary>
    /// Shows an <see cref="SntvReceiver"/>'s video on this object's renderer (a quad, a plane, a
    /// curved screen) with the "Sntv/StereoSideBySide" shader: mono streams as they are, stereo
    /// streams with the left half to the left eye and the right half to the right one. Scales the
    /// object to the aspect ratio of one view.
    ///
    /// A flat screen is the simplest way to look at the stream. For immersive fisheye video (the
    /// headset looking around inside it), map the stream's projection onto a sphere or dome with
    /// your own shader: <see cref="SntvReceiver.Info"/> carries the lens and field of view.
    /// </summary>
    [RequireComponent(typeof(Renderer))]
    public class SntvDisplay : MonoBehaviour
    {
        public SntvReceiver receiver;
        [Tooltip("Swap which half goes to which eye.")]
        public bool swapEyes;
        [Tooltip("Keep the object's height and set its width from one view's aspect ratio.")]
        public bool fitAspect = true;
        [Tooltip("Sntv/StereoSideBySide. Kept as a reference so builds include it (Shader.Find " +
                 "alone fails in a player when nothing else uses the shader).")]
        public Shader shader;

        static readonly int k_MainTex = Shader.PropertyToID("_MainTex");
        static readonly int k_Stereo = Shader.PropertyToID("_Stereo");
        static readonly int k_Swap = Shader.PropertyToID("_SwapEyes");

        Material m_Material;

        void OnEnable()
        {
            if (receiver == null) receiver = FindAnyObjectByType<SntvReceiver>();
            if (receiver == null) { Debug.LogWarning("[SNTV] SntvDisplay without a receiver."); return; }
            var r = GetComponent<Renderer>();
            if (shader == null) shader = Shader.Find("Sntv/StereoSideBySide");
            m_Material = new Material(shader);
            r.material = m_Material;
            receiver.FirstFrame += OnFirstFrame;
            if (receiver.HasVideo) OnFirstFrame(receiver.Texture);
        }

        void Reset()
        {
            shader = Shader.Find("Sntv/StereoSideBySide");
        }

        void OnDisable()
        {
            if (receiver != null) receiver.FirstFrame -= OnFirstFrame;
        }

        void OnFirstFrame(RenderTexture tex)
        {
            var info = receiver.Info;
            m_Material.SetTexture(k_MainTex, tex);
            m_Material.SetFloat(k_Stereo, info.stereo ? 1f : 0f);
            m_Material.SetFloat(k_Swap, swapEyes ? 1f : 0f);
            if (fitAspect && info.height > 0)
            {
                float aspect = (float)info.width / info.Views / info.height;
                var s = transform.localScale;
                transform.localScale = new Vector3(s.y * aspect, s.y, s.z);
            }
        }

        void OnDestroy()
        {
            if (m_Material != null) Destroy(m_Material);
        }
    }
}
