//
// Copyright (C) YuqiaoZhang(HanetakaChou)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#ifndef _BRX_HEMISPHERICAL_DIRECTIONAL_REFLECTANCE_INTEGRATION_H_
#define _BRX_HEMISPHERICAL_DIRECTIONAL_REFLECTANCE_INTEGRATION_H_ 1

#include <DirectXMath.h>
#include <cmath>
#include <algorithm>
#include "../../Brioche-Shader-Language/include/brx_low_discrepancy_sequence.h"
#include "../../Brioche-Shader-Language/include/brx_brdf.h"
#include "../../McRT-Malloc/include/mcrt_parallel_map.h"
#include "../../McRT-Malloc/include/mcrt_parallel_reduce.h"

// UE4：[128](https://github.com/EpicGames/UnrealEngine/blob/4.27/Engine/Source/Runtime/Renderer/Private/SystemTextures.cpp#L322)
// U3D: [4096](https://github.com/Unity-Technologies/Graphics/blob/v10.8.1/com.unity.render-pipelines.core/ShaderLibrary/ImageBasedLighting.hlsl#L340)
static constexpr uint32_t const INTERNAL_BRX_HDR_INTEGRATION_MONTE_CARLO_SAMPLE_COUNT = 16384U;

static constexpr uint32_t const INTERNAL_BRX_HDR_INTEGRATION_MONTE_CARLO_GRAIN_SIZE = 64U;

static constexpr float const INTERNAL_BRX_HDR_INTEGRATION_LENGTH_MINIMUM = 1E-5F;

static inline void internal_brx_hemispherical_directional_reflectance_compute_norm(uint32_t const lut_width_index, uint32_t const lut_height_index, uint32_t const lut_width, uint32_t const lut_height, DirectX::XMFLOAT2 &out_norm);

static inline void brx_hemispherical_directional_reflectance_compute_norms(DirectX::XMFLOAT2 *const out_lut, uint32_t const lut_width, uint32_t const lut_height)
{
    // UE4: [128x32](https://github.com/EpicGames/UnrealEngine/blob/4.27/Engine/Source/Runtime/Renderer/Private/SystemTextures.cpp#L289)
    // U3D: [64x64] (https://github.com/Unity-Technologies/Graphics/blob/v10.8.1/com.unity.render-pipelines.high-definition/Runtime/Material/PreIntegratedFGD/PreIntegratedFGD.cs.hlsl#L10)

    struct parallel_map_user_data
    {
        DirectX::XMFLOAT2 *const out_lut;
        uint32_t const lut_width;
        uint32_t const lut_height;
    };

    parallel_map_user_data user_data = {out_lut, lut_width, lut_height};

    mcrt_parallel_map(
        0U,
        lut_height * lut_width,
        1U,
        [](uint32_t begin, uint32_t end, void *wrapped_user_data) -> void
        {
            parallel_map_user_data *unwrapped_user_data = static_cast<parallel_map_user_data *>(wrapped_user_data);
            DirectX::XMFLOAT2 *const out_lut = unwrapped_user_data->out_lut;
            uint32_t const lut_width = unwrapped_user_data->lut_width;
            uint32_t const lut_height = unwrapped_user_data->lut_height;

            assert((begin + 1U) == end);
            uint32_t const lut_width_index = (begin % lut_width);
            uint32_t const lut_height_index = (begin / lut_width);

            DirectX::XMFLOAT2 norm;
            internal_brx_hemispherical_directional_reflectance_compute_norm(lut_width_index, lut_height_index, lut_width, lut_height, norm);
            out_lut[lut_width * lut_height_index + lut_width_index] = norm;
        },
        &user_data);
}

static inline void internal_brx_hemispherical_directional_reflectance_compute_fresnel_factor_zeroth_spherical_moment(float const raw_alpha, DirectX::XMFLOAT3 const &raw_omega_o, float &out_f0_factor_zeroth_spherical_moment, float &out_f90_factor_zeroth_spherical_moment)
{
    assert(raw_alpha >= BRX_TROWBRIDGE_REITZ_ALPHA_MINIMUM);
    float const alpha = std::max(BRX_TROWBRIDGE_REITZ_ALPHA_MINIMUM, raw_alpha);

    assert(raw_omega_o.z >= BRX_TROWBRIDGE_REITZ_NDOTV_MINIMUM);
    DirectX::XMFLOAT3 const omega_o(raw_omega_o.x, raw_omega_o.y, std::max(BRX_TROWBRIDGE_REITZ_NDOTV_MINIMUM, raw_omega_o.z));

    struct parallel_reduce_user_data
    {
        DirectX::XMFLOAT3 const omega_o;
        float const alpha;
    };

    parallel_reduce_user_data user_data = {omega_o, alpha};

    mcrt_double2 fresnel_factor_zeroth_spherical_moment = mcrt_parallel_reduce_double2(
        0U,
        INTERNAL_BRX_HDR_INTEGRATION_MONTE_CARLO_SAMPLE_COUNT,
        INTERNAL_BRX_HDR_INTEGRATION_MONTE_CARLO_GRAIN_SIZE,
        [](uint32_t begin, uint32_t end, void *wrapped_user_data) -> mcrt_double2
        {
            parallel_reduce_user_data *unwrapped_user_data = static_cast<parallel_reduce_user_data *>(wrapped_user_data);
            DirectX::XMFLOAT3 const omega_o = unwrapped_user_data->omega_o;
            float const alpha = unwrapped_user_data->alpha;

            double f0_factor_zeroth_spherical_moment = 0.0;
            double f90_factor_zeroth_spherical_moment = 0.0;

            for (uint32_t sample_index = begin; sample_index < end; ++sample_index)
            {
                DirectX::XMFLOAT2 xi = brx_hammersley_2d(sample_index, INTERNAL_BRX_HDR_INTEGRATION_MONTE_CARLO_SAMPLE_COUNT);

                DirectX::XMFLOAT3 omega_h = brx_trowbridge_reitz_sample_omega_h(xi, alpha, omega_o);

                DirectX::XMFLOAT3 omega_i;
                {
                    DirectX::XMStoreFloat3(&omega_i, DirectX::XMVector3Normalize(DirectX::XMVector3Reflect(DirectX::XMVectorNegate(DirectX::XMLoadFloat3(&omega_o)), DirectX::XMLoadFloat3(&omega_h))));
                }

                float NdotL = omega_i.z;

                float NdotV = omega_o.z;

                float VdotH;
                {
                    float VdotL = DirectX::XMVectorGetX(DirectX::XMVector3Dot(DirectX::XMLoadFloat3(&omega_o), DirectX::XMLoadFloat3(&omega_i)));

                    // Real-Time Rendering Fourth Edition / 9.8 BRDF Models for Surface Reflection / [Hammon 2017]
                    // UE4: [Init](https://github.com/EpicGames/UnrealEngine/blob/4.27/Engine/Shaders/Private/BRDF.ush#L31)
                    // U3D: [GetBSDFAngle](https://github.com/Unity-Technologies/Graphics/blob/v10.8.0/com.unity.render-pipelines.core/ShaderLibrary/CommonLighting.hlsl#L361)
                    float invLenH = 1.0F / std::sqrt(std::max(INTERNAL_BRX_HDR_INTEGRATION_LENGTH_MINIMUM, 2.0F + 2.0F * VdotL));
                    VdotH = std::min(std::max(0.0F, invLenH * VdotL + invLenH), 1.0F);
                }

                NdotL = std::max(0.0F, NdotL);

                NdotV = std::max(0.0F, NdotV);

                float monochromatic_throughput = brx_trowbridge_reitz_throughput_without_fresnel(alpha, NdotL);

                float f0_factor;
                float f90_factor;
                {
                    // glTF Sample Renderer: [F_Schlick](https://github.com/KhronosGroup/glTF-Sample-Renderer/blob/e5646a2bf87b0871ba3f826fc2335fe117a11411/source/Renderer/shaders/brdf.glsl#L24)

                    float x = std::min(std::max(0.0F, 1.0F - VdotH), 1.0F);
                    float x2 = x * x;
                    float x5 = x * x2 * x2;

                    f0_factor = 1.0F - x5;
                    f90_factor = x5;
                }

                assert(monochromatic_throughput >= 0.0F);
                f0_factor_zeroth_spherical_moment += ((1.0 / static_cast<double>(INTERNAL_BRX_HDR_INTEGRATION_MONTE_CARLO_SAMPLE_COUNT)) * static_cast<double>(monochromatic_throughput) * static_cast<double>(f0_factor));
                f90_factor_zeroth_spherical_moment += ((1.0 / static_cast<double>(INTERNAL_BRX_HDR_INTEGRATION_MONTE_CARLO_SAMPLE_COUNT)) * static_cast<double>(monochromatic_throughput) * static_cast<double>(f90_factor));
            }

            return mcrt_double2{f0_factor_zeroth_spherical_moment, f90_factor_zeroth_spherical_moment};
        },
        &user_data);

    out_f0_factor_zeroth_spherical_moment = static_cast<float>(fresnel_factor_zeroth_spherical_moment.x);
    out_f90_factor_zeroth_spherical_moment = static_cast<float>(fresnel_factor_zeroth_spherical_moment.y);
}

static inline void internal_brx_hemispherical_directional_reflectance_compute_norm(uint32_t const lut_width_index, uint32_t const lut_height_index, uint32_t const lut_width, uint32_t const lut_height, DirectX::XMFLOAT2 &out_norm)
{
    // Remap: [0, 1] -> [0.5/size, 1.0 - 0.5/size]
    // U3D: [Remap01ToHalfTexelCoord](https://github.com/Unity-Technologies/Graphics/blob/v10.8.0/com.unity.render-pipelines.core/ShaderLibrary/Common.hlsl#L661)
    // UE4: [N/A](https://github.com/EpicGames/UnrealEngine/blob/4.27/Engine/Shaders/Private/RectLight.ush#L450)

    assert(lut_width_index < lut_width);
    float texcoord_u = static_cast<float>(lut_width_index) / static_cast<float>(lut_width - 1U);

    assert(lut_height_index < lut_height);
    float texcoord_v = static_cast<float>(lut_height_index) / static_cast<float>(lut_height - 1U);

    // u = 1 - cos_theta
    // cos_theta = 1 - u
    DirectX::XMFLOAT3 omega_o;
    {
        float const cos_theta_o = std::max(BRX_TROWBRIDGE_REITZ_NDOTV_MINIMUM, 1.0F - texcoord_u);
        omega_o = DirectX::XMFLOAT3(std::sqrt(std::max(0.0F, 1.0F - cos_theta_o * cos_theta_o)), 0.0F, cos_theta_o);
    }

    // v = 1 - alpha
    // alpha = 1 - v
    float const alpha = std::max(BRX_TROWBRIDGE_REITZ_ALPHA_MINIMUM, 1.0F - texcoord_v);

    float f0_factor_zeroth_spherical_moment;
    float f90_factor_zeroth_spherical_moment;
    internal_brx_hemispherical_directional_reflectance_compute_fresnel_factor_zeroth_spherical_moment(alpha, omega_o, f0_factor_zeroth_spherical_moment, f90_factor_zeroth_spherical_moment);

    out_norm = DirectX::XMFLOAT2(f0_factor_zeroth_spherical_moment, f90_factor_zeroth_spherical_moment);
}

#endif
