/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "checkMacrosPlugin.h"
#include "tensorrt_llm/common/cublasMMWrapper.h"
#include "tensorrt_llm/common/logger.h"
#include "tensorrt_llm/common/quantization.h"
#include "tensorrt_llm/common/stringUtils.h"
#include "tensorrt_llm/kernels/contextFusedMultiHeadAttention/fmhaRunner.h"
#include "tensorrt_llm/kernels/contextFusedMultiHeadAttention/fused_multihead_attention_common.h"
#include "tensorrt_llm/kernels/gptKernels.h"
#include "tensorrt_llm/plugins/common/plugin.h"
#include "tensorrt_llm/plugins/gptAttentionCommon/gptAttentionCommon.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace tensorrt_llm::plugins
{
// batch_size = num_ctx_requests + num_gen_requests * beam_width
// num_ctx_requests = number of context requests (single sequence per request).
// num_gen_requests = number of generation requests (beam_width sequences per request).
// Context sequences have to appear first, generation sequences after

// inputs (see GPTAttentionPlugin::isEntryUsed for when each tensor is actually used)
//     0.  input_tensor [batch_size, seq_len, local_hidden_size + 2 * local_num_kv_heads * head_size] or
//                      [num_tokens, local_hidden_size + 2 * local_num_kv_heads * head_size] when
//                      enable_remove_input_padding
//     1.  sequence_length [batch_size] (optional)
//     2.  host_past_key_value_lengths [batch_size] (int32) (optional)
//     3.  host_max_attention_window_sizes [num_layers] (int32)
//     4.  host_sink_token_length [1] (int32)
//     5.  context_lengths [batch_size]
//     6.  cache_indir [num_gen_requests, beam_width, memory_max_len] (required in beamsearch) (optional)
//     7.  host_request_types [batch_size] int32. 0: context; 1: generation: 2: none. When not in inflight-batching
//     mode,
//                      all elements must be identical.
//     8.  past_key_value_pool [batch_size, 2, local_num_kv_heads, max_seq_len, head_size] or
//         block_offsets [batch_size, 2, max_blocks_per_seq] if paged kv cache (optional)
//     8.1 host_block_offsets [batch_size, 2, max_blocks_per_seq] if paged kv cache (optional)
//     8.2 host_pool_pointers [2] if paged kv cache (optional)
//     9.  kv_cache_quantization_scale [1] (optional)
//     10. kv_cache_dequantization_scale [1] (optional)
//     11. attention_output_quantization_scale [1] (on device, optional)
//     12. rotary_cos_sin [max_num_embedding_positions, 2] (float) (on device, optional)
//     13. alibi_slopes [num_heads] (optional for ALiBi position embedding)
//     14. relative_attention_bias [num_heads] (optional for ALiBi position embedding)
//     15. host_context_lengths [batch_size] int32. (optional, required when remove_input_padding is true)
//     16. qkv_bias (optional) [local_hidden_size * 3]
//     17. spec_decoding_generation_lengths (optional, required when medusa is enabled) (int32_t) [batch_size]
//     18. spec_decoding_packed_mask (optional, required when medusa is enabled) (int32_t) [num_tokens, packed_mask_dim]
//                                    packed_mask_dim = divUp(max_num_spec_decoding_tokens + 1, 32)
//     19. spec_decoding_position_offsets (optional, required when medusa is enabled) (int32_t) [batch_size,
//     max_num_spec_decoding_tokens + 1]
//
// outputs
//     output_tensor [batch_size, seq_len, local_hidden_size]
//     present_key_value_pool (optional if not paged kv cache) [batch_size, 2, local_num_kv_heads, max_seq_len,
//     head_size]

class GPTAttentionPlugin : public GPTAttentionPluginCommon
{
public:
    GPTAttentionPlugin(int layer_idx, int num_heads, int vision_start, int vision_length, int num_kv_heads,
        int head_size, int unidirectional, float q_scaling, float qk_tanh_scale,
        tensorrt_llm::kernels::PositionEmbeddingType position_embedding_type,
        int rotary_embedding_dim, // for RoPE. 0 for non-RoPE
        float rotary_embedding_base, tensorrt_llm::kernels::RotaryScalingType rotary_embedding_scale_type,
        float rotary_embedding_scale, float rotary_embedding_short_m_scale, float rotary_embedding_long_m_scale,
        int rotary_embedding_max_positions, int rotary_embedding_original_max_positions, int tp_size,
        int tp_rank,          // for ALiBi
        bool unfuse_qkv_gemm, // for AutoPP
        tensorrt_llm::kernels::ContextFMHAType context_fmha_type, bool multi_block_mode, bool enable_xqa,
        int kv_cache_quant_mode, bool remove_input_padding, tensorrt_llm::kernels::AttentionMaskType mask_type,
        tensorrt_llm::kernels::BlockSparseParams block_sparse_params, bool paged_kv_cache, int tokens_per_block,
        nvinfer1::DataType type, int32_t max_context_length, bool qkv_bias_enabled, bool cross_attention = false,
        int max_distance = 0, bool pos_shift_enabled = false, bool dense_context_fmha = false,
        bool use_paged_context_fmha = false, bool use_fp8_context_fmha = false, bool use_cache = true,
        bool is_spec_decoding_enabled = false, bool spec_decoding_is_generation_length_variable = false,
        int spec_decoding_max_generation_length = 1);

    GPTAttentionPlugin(void const* data, size_t length);

    ~GPTAttentionPlugin() override = default;

    // IPluginV2DynamicExt Methods
    nvinfer1::DimsExprs getOutputDimensions(int outputIndex, nvinfer1::DimsExprs const* inputs, int nbInputs,
        nvinfer1::IExprBuilder& exprBuilder) noexcept override;

    bool supportsFormatCombination(
        int pos, nvinfer1::PluginTensorDesc const* inOut, int nbInputs, int nbOutputs) noexcept override;
    size_t getWorkspaceSize(nvinfer1::PluginTensorDesc const* inputs, int nbInputs,
        nvinfer1::PluginTensorDesc const* outputs, int nbOutputs) const noexcept override;
    int enqueue(nvinfer1::PluginTensorDesc const* inputDesc, nvinfer1::PluginTensorDesc const* outputDesc,
        void const* const* inputs, void* const* outputs, void* workspace, cudaStream_t stream) noexcept override;

    template <typename T, typename AttentionOutT, typename KVCacheBuffer>
    int enqueueImpl(nvinfer1::PluginTensorDesc const* inputDesc, nvinfer1::PluginTensorDesc const* outputDesc,
        void const* const* inputs, void* const* outputs, void* workspace, cudaStream_t stream);

    template <typename T, typename AttentionOutT = T>
    int enqueueDispatchKVCacheType(nvinfer1::PluginTensorDesc const* inputDesc,
        nvinfer1::PluginTensorDesc const* outputDesc, void const* const* inputs, void* const* outputs, void* workspace,
        cudaStream_t stream);

    template <typename T, typename KVCacheBuffer>
    void configurePluginImpl(nvinfer1::DynamicPluginTensorDesc const* in, int nbInputs,
        nvinfer1::DynamicPluginTensorDesc const* out, int nbOutputs) noexcept;
    template <typename T>
    void configurePluginDispatchKVCacheType(nvinfer1::DynamicPluginTensorDesc const* in, int nbInputs,
        nvinfer1::DynamicPluginTensorDesc const* out, int nbOutputs) noexcept;
    void configurePlugin(nvinfer1::DynamicPluginTensorDesc const* in, int nbInputs,
        nvinfer1::DynamicPluginTensorDesc const* out, int nbOutputs) noexcept override;

    // IPluginV2Ext Methods
    nvinfer1::DataType getOutputDataType(
        int index, nvinfer1::DataType const* inputTypes, int nbInputs) const noexcept override;

    // IPluginV2 Methods
    char const* getPluginType() const noexcept override;
    char const* getPluginVersion() const noexcept override;
    int getNbOutputs() const noexcept override;

    //! This is called on every trt ExecutionContext creation by TRT
    //! Note TRT does not call the initialize on cloned plugin, so clone internally should do initialization.
    GPTAttentionPlugin* clone() const noexcept override;

    size_t getSerializationSize() const noexcept override;
    void serialize(void* buffer) const noexcept override;

    enum class RequestType : int32_t
    {
        kCONTEXT = 0,
        kGENERATION = 1
    };

private:
    template <typename T, typename AttentionOutT, typename KVCacheBuffer>
    int enqueueSome(int32_t seqIdxBeg, int32_t localNbSeq, int32_t tokenIdxBeg, int32_t localNbTokens,
        nvinfer1::PluginTensorDesc const* inputDesc, nvinfer1::PluginTensorDesc const* outputDesc,
        void const* const* inputs, void* const* outputs, void* workspace, cudaStream_t stream);

    using IndexType = std::int32_t;

    std::vector<size_t> mEntryIdx;
    enum class IdxEntry : size_t
    {
        QKV_TENSOR,
        K_TENSOR,
        V_TENSOR,
        SEQUENCE_LENGTH,
        HOST_PAST_KEY_VALUE_LENGTHS,
        HOST_MAX_ATTENTION_WINDOW,
        HOST_SINK_TOKEN_LENGTH,
        CONTEXT_LENGTHS,
        CACHE_INDIR,
        REQUEST_TYPES,
        KV_CACHE_BLOCK_OFFSETS,
        HOST_KV_CACHE_BLOCK_OFFSETS,
        HOST_KV_CACHE_POOL_POINTERS,
        PAST_KEY_VALUE,
        KV_CACHE_QUANTIZATION_SCALE,
        KV_CACHE_DEQUANTIZATION_SCALE,
        ATTENTION_OUTPUT_QUANTIZATION_SCALE,
        ROTARY_COS_SIN,
        ROTARY_EMBEDDING_SCALING_FACTORS,
        ALIBI_SLOPES,
        RELATIVE_ATTENTION_BIAS,
        CROSS_QKV,
        CROSS_QKV_LENGTH,
        ENCODER_INPUT_LENGTH,
        HOST_CONTEXT_LENGTH,
        QKV_BIAS_TENSOR,
        SPEC_DECODING_GENERATION_LENGTHS,
        SPEC_DECODING_PACKED_MASK,
        SPEC_DECODING_POSITION_OFFSETS,
        ENUM_SIZE,
    };

    bool isEntryUsed(IdxEntry const& entry) const;
    void initEntryIdx();
    IndexType getIdx(IdxEntry const& entry) const;

    // Get generation input sequence length (might be larger than 1 in the speculative decoding mode).
    int getGenerationInputSequenceLength(
        nvinfer1::PluginTensorDesc const* inputDesc, int32_t localNbSeq, int32_t localNbTokens) const;
};

class GPTAttentionPluginCreator : public GPTAttentionPluginCreatorCommon
{
public:
    GPTAttentionPluginCreator();

    char const* getPluginName() const noexcept override;

    char const* getPluginVersion() const noexcept override;

    nvinfer1::PluginFieldCollection const* getFieldNames() noexcept override;

    nvinfer1::IPluginV2* createPlugin(char const* name, nvinfer1::PluginFieldCollection const* fc) noexcept override;

    nvinfer1::IPluginV2* deserializePlugin(
        char const* name, void const* serialData, size_t serialLength) noexcept override;
};

} // namespace tensorrt_llm::plugins
