/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file mkldnn_quantized_batch_norm.cc
 * \brief
 * \author Yixin Bao
 */

#if MXNET_USE_MKLDNN == 1
#include "../../nn/mkldnn/mkldnn_batch_norm-inl.h"
#include "../quantization_utils.h"

namespace mxnet {
namespace op {

static void MKLDNNQuantizedBatchNormForward(const nnvm::NodeAttrs& attrs,
                                            const OpContext& ctx,
                                            const std::vector<NDArray>& in_data,
                                            const std::vector<OpReqType>& req,
                                            const std::vector<NDArray>& outputs) {
  CHECK_EQ(in_data.size(), 7U);
  CHECK_EQ(outputs.size(), 3U);

  TmpMemMgr::Get()->Init(ctx.requested[batchnorm::kTempSpace]);
  const BatchNormParam& param = nnvm::get<BatchNormParam>(attrs.parsed);
  const NDArray& data         = in_data[quantized_batchnorm::kData];
  auto data_mem          = static_cast<const mkldnn::memory*>(data.GetMKLDNNData());

  // reorder if data type = uint8
  if (in_data[quantized_batchnorm::kData].dtype() == mshadow::kUint8) {
    auto u8_md            = data_mem->get_desc();
    auto s8_md            = u8_md;
    s8_md.data.data_type  = static_cast<mkldnn_data_type_t>(mkldnn::memory::data_type::s8);
    auto data_reorder_mem = TmpMemMgr::Get()->Alloc(s8_md);

    std::vector<float> reorder_scale;
    reorder_scale = {static_cast<float>(kInt8Range) / kUint8Range};
    mkldnn::primitive_attr reorder_attr;
    reorder_attr.set_output_scales(0, reorder_scale);
    mkldnn::engine cpu_engine = CpuEngine::Get()->get_engine();
    const auto reorder_pd =
        mkldnn::reorder::primitive_desc(cpu_engine, u8_md, cpu_engine, s8_md, reorder_attr);
    mkldnn_args_map_t reorder_args;
    reorder_args[MKLDNN_ARG_SRC] = *data_mem;
    reorder_args[MKLDNN_ARG_DST] = *data_reorder_mem;
    MKLDNNStream::Get()->RegisterPrimArgs(mkldnn::reorder(reorder_pd), reorder_args);
    data_mem = data_reorder_mem;
  }
  const size_t channelAxis = static_cast<size_t>(
      param.axis < 0 ? static_cast<int>(data.shape().ndim()) + param.axis : param.axis);
  const int channel_count  = data.shape()[channelAxis];
  const float min_data     = in_data[quantized_batchnorm::kDataMin].data().dptr<float>()[0];
  const float max_data     = in_data[quantized_batchnorm::kDataMax].data().dptr<float>()[0];
  const float max_abs_data = std::max(std::abs(min_data), std::abs(max_data));

  float* min_output_ptr = outputs[quantized_batchnorm::kOutMin].data().dptr<float>();
  float* max_output_ptr = outputs[quantized_batchnorm::kOutMax].data().dptr<float>();
  if (param.min_calib_range.has_value() && param.max_calib_range.has_value()) {
    *max_output_ptr = param.max_calib_range.value();
    *min_output_ptr = param.min_calib_range.value();
  } else {
    LOG(FATAL) << "min_calib_range or max_calib_range is not available. "
                  "Quantized BN currently "
                  "don't support calib_mode=None";
  }
  const float max_abs_output = std::max(std::abs(*min_output_ptr), std::abs(*max_output_ptr));

  mkldnn::normalization_flags flags =
      mkldnn::normalization_flags::use_global_stats | mkldnn::normalization_flags::use_scale_shift;
  auto& fwd                        = GetBNForward<float>(param, ctx, data_mem, flags);
  const mkldnn::memory& weight_mem = fwd.GetWeight();
  CHECK_EQ(weight_mem.get_desc().get_size(), channel_count * sizeof(float) * 2);
  float* weight_buf = reinterpret_cast<float*>(weight_mem.get_data_handle());

  float* gamma_ptr = in_data[quantized_batchnorm::kGamma].data().dptr<float>();
  float* beta_ptr  = in_data[quantized_batchnorm::kBeta].data().dptr<float>();

  const NDArray& moving_mean = in_data[quantized_batchnorm::kInMovingMean];
  const NDArray& moving_var  = in_data[quantized_batchnorm::kInMovingVar];
  float* moving_mean_ptr     = moving_mean.data().dptr<float>();
  float* moving_var_ptr      = moving_var.data().dptr<float>();

  // rescale gamma and beta, to make mean=0 and var=1
  auto rescaled_mean_mem = TmpMemMgr::Get()->Alloc(
      static_cast<const mkldnn::memory*>(moving_mean.GetMKLDNNData())->get_desc());
  auto rescaled_var_mem = TmpMemMgr::Get()->Alloc(
      static_cast<const mkldnn::memory*>(moving_var.GetMKLDNNData())->get_desc());
  float* rescaled_mean_ptr = reinterpret_cast<float*>(rescaled_mean_mem->get_data_handle());
  float* rescaled_var_ptr  = reinterpret_cast<float*>(rescaled_var_mem->get_data_handle());

#pragma omp parallel for num_threads(engine::OpenMP::Get()->GetRecommendedOMPThreadCount())
  for (int channel = 0; channel < channel_count; ++channel) {
    float invstd        = 1.0 / std::sqrt(moving_var_ptr[channel] + param.eps);
    weight_buf[channel] = gamma_ptr[channel] * invstd * max_abs_data / max_abs_output;
    weight_buf[channel_count + channel] =
        (beta_ptr[channel] - moving_mean_ptr[channel] * gamma_ptr[channel] * invstd) * kInt8Range /
        max_abs_output;
    rescaled_mean_ptr[channel] = 0.0f;
    rescaled_var_ptr[channel]  = 1.0f;
  }

  const NDArray& out = outputs[batchnorm::kOut];
  auto fwd_dst_desc = fwd.GetPd().dst_desc();
  auto out_mem =
      static_cast<mkldnn::memory*>(const_cast<NDArray&>(out).CreateMKLDNNData(&fwd_dst_desc));
  mkldnn_args_map_t net_args;
  net_args[MKLDNN_ARG_SRC]         = *data_mem;
  net_args[MKLDNN_ARG_SCALE_SHIFT] = weight_mem;
  net_args[MKLDNN_ARG_DST]         = *out_mem;
  net_args[MKLDNN_ARG_MEAN]        = *rescaled_mean_mem;
  net_args[MKLDNN_ARG_VARIANCE]    = *rescaled_var_mem;

  MKLDNNStream::Get()->RegisterPrimArgs(fwd.GetFwd(), net_args);
  MKLDNNStream::Get()->Submit();
}

inline static bool QuantizedBatchNormStorageType(const nnvm::NodeAttrs& attrs,
                                                 const int dev_mask,
                                                 DispatchMode* dispatch_mode,
                                                 std::vector<int>* in_attrs,
                                                 std::vector<int>* out_attrs) {
  bool dispatched = false;
  if (!dispatched) {
    dispatched = MKLDNNStorageType(attrs, dev_mask, true, dispatch_mode, in_attrs, out_attrs);
  }
  return dispatched;
}

NNVM_REGISTER_OP(_contrib_quantized_batch_norm)
    .set_attr<FInferStorageType>("FInferStorageType", QuantizedBatchNormStorageType)
    .set_attr<FComputeEx>("FComputeEx<cpu>", MKLDNNQuantizedBatchNormForward)
    .set_attr<FResourceRequest>("FResourceRequest",
                                [](const NodeAttrs& n) {
                                  return std::vector<ResourceRequest>{ResourceRequest::kTempSpace};
                                })
    .set_attr<bool>("TIsMKLDNN", true);

}  // namespace op
}  // namespace mxnet

#endif  // MXNET_USE_MKLDNN == 1
