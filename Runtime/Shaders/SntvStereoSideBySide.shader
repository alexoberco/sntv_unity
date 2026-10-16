// Copyright 2026 Alejandro Bermúdez Fajardo (alexoberco)
// SPDX-License-Identifier: Apache-2.0
//
// Unlit display of an SNTV texture. With _Stereo = 1 each eye samples its own half of a
// side-by-side frame (left view on the left). Works in the built-in pipeline and in URP, with
// multi-pass, single-pass and single-pass instanced stereo.
Shader "Sntv/StereoSideBySide"
{
    Properties
    {
        _MainTex ("Video", 2D) = "black" {}
        [Toggle] _Stereo ("Stereo side by side", Float) = 0
        [Toggle] _SwapEyes ("Swap eyes", Float) = 0
    }
    SubShader
    {
        Tags { "RenderType" = "Opaque" "Queue" = "Geometry" }
        Pass
        {
            Cull Off
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float _Stereo, _SwapEyes;

            struct appdata
            {
                float4 vertex : POSITION;
                float2 uv : TEXCOORD0;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            struct v2f
            {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.uv;
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
                float2 uv = i.uv;
                if (_Stereo > 0.5)
                {
                    float eye = (float)unity_StereoEyeIndex;
                    if (_SwapEyes > 0.5) eye = 1.0 - eye;
                    uv.x = uv.x * 0.5 + eye * 0.5;
                }
                return fixed4(tex2D(_MainTex, uv).rgb, 1.0);
            }
            ENDCG
        }
    }
}
