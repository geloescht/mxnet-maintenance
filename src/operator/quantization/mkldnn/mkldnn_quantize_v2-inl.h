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
 * \file mkldnn_quantize_v2-inl.h
 * \brief
 */

#ifndef MXNET_OPERATOR_QUANTIZATION_MKLDNN_MKLDNN_QUANTIZE_V2_INL_H_
#define MXNET_OPERATOR_QUANTIZATION_MKLDNN_MKLDNN_QUANTIZE_V2_INL_H_
#if MXNET_USE_MKLDNN == 1
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../../nn/mkldnn/mkldnn_base-inl.h"
#include "../quantize_v2-inl.h"

namespace mxnet {
namespace op {

class SgMKLDNNQuantizeOperator {
 public:
  explicit SgMKLDNNQuantizeOperator(const nnvm::NodeAttrs& attrs)
      : param_(nnvm::get<QuantizeV2Param>(attrs.parsed)) {}

  void Forward(const OpContext& ctx,
               const std::vector<NDArray>& inputs,
               const std::vector<OpReqType>& req,
               const std::vector<NDArray>& outputs);

 private:
  bool initalized_{false};
  QuantizeV2Param param_;
  float cached_data_min_{0.f};
  float cached_data_max_{0.f};
  float cached_scale_;
  uint8_t cached_shift_{0};
  mkldnn::memory::desc o_desc_;
  mkldnn_args_map_t args_;
  std::shared_ptr<mkldnn::reorder> fwd_pd_;
};

void SgMKLDNNQuantizeOperator::Forward(const OpContext& ctx,
                                       const std::vector<NDArray>& inputs,
                                       const std::vector<OpReqType>& req,
                                       const std::vector<NDArray>& outputs) {
  NDArray in_buffer = inputs[0];
  float data_min    = mshadow::red::limits::MaxValue<float>();
  float data_max    = mshadow::red::limits::MinValue<float>();

  // Pass through quantized data
  if (inputs[0].dtype() == mshadow::kUint8 || inputs[0].dtype() == mshadow::kInt8) {
    if (param_.min_calib_range.has_value() && param_.max_calib_range.has_value()) {
      *outputs[1].data().dptr<float>() = param_.min_calib_range.value();
      *outputs[2].data().dptr<float>() = param_.max_calib_range.value();
    } else {
      if (inputs[0].dtype() == mshadow::kUint8) {
        *outputs[1].data().dptr<float>() = 0;
        *outputs[2].data().dptr<float>() = kUint8Range;
      } else {
        *outputs[1].data().dptr<float>() = -kInt8Range;
        *outputs[2].data().dptr<float>() = kInt8Range;
      }
    }
    if (req[0] != kWriteInplace) {
      const_cast<NDArray&>(outputs[0])
          .CopyFrom(static_cast<const mkldnn::memory*>(inputs[0].GetMKLDNNData()));
      MKLDNNStream::Get()->Submit();
    }
  } else {
    if (in_buffer.IsView() && in_buffer.IsMKLDNNData())
      in_buffer = inputs[0].Reorder2Default();
    auto i_mem = static_cast<const mkldnn::memory*>(in_buffer.GetMKLDNNData());

    if (param_.min_calib_range.has_value() && param_.max_calib_range.has_value()) {
      data_min = param_.min_calib_range.value();
      data_max = param_.max_calib_range.value();
    } else {
      // no calib info
      in_buffer     = inputs[0].Reorder2Default();
      auto in_ptr   = in_buffer.data().dptr<float>();
      auto nthreads = engine::OpenMP::Get()->GetRecommendedOMPThreadCount();
      std::vector<float> data_maxs(nthreads, data_max);
      std::vector<float> data_mins(nthreads, data_min);
#pragma omp parallel for num_threads(nthreads)
      for (index_t i = 0; i < static_cast<index_t>(in_buffer.shape().Size()); i++) {
        int tid = omp_get_thread_num();
        if (in_ptr[i] > data_maxs[tid])
          data_maxs[tid] = in_ptr[i];
        if (in_ptr[i] < data_mins[tid])
          data_mins[tid] = in_ptr[i];
      }
      for (index_t i = 0; i < nthreads; i++) {
        if (data_maxs[i] > data_max)
          data_max = data_maxs[i];
        if (data_mins[i] < data_min)
          data_min = data_mins[i];
      }

      if (initalized_ && (cached_data_min_ != data_min || cached_data_max_ != data_max))
        initalized_ = false;
    }

    // Write output min/max
    auto out_type             = GetQuantizeOutputType(param_);
    const bool shifted_output = param_.shifted_output.has_value() && param_.shifted_output.value();
    if (shifted_output) {
      // if shifted_output == true we have guarantee that data_min is negative because
      // we require that in asymmetric quantization pass in quantize_graph_pass
      // Modify out min/max range to reflect shifted data
      out_type                         = mshadow::kUint8;
      *outputs[1].data().dptr<float>() = 0;
      *outputs[2].data().dptr<float>() = data_max - data_min;
    } else if (out_type == mshadow::kUint8) {
      *outputs[1].data().dptr<float>() = data_min;
      *outputs[2].data().dptr<float>() = data_max;
    } else if (out_type == mshadow::kInt8) {
      float real_range                 = MaxAbs(data_min, data_max);
      *outputs[1].data().dptr<float>() = -real_range;
      *outputs[2].data().dptr<float>() = real_range;
    } else {
      LOG(FATAL) << "mkldnn quantize op only supports int8 and uint8 as output type";
    }

    if (!initalized_) {
      cached_data_min_ = data_min;
      cached_data_max_ = data_max;
      if (shifted_output) {
        CHECK_LT(data_min, 0);  // assert that we are working on signed
        cached_scale_ = kUint8Range / (data_max - data_min);
        cached_shift_ = static_cast<uint8_t>(std::round(cached_scale_ * -cached_data_min_));
      } else {
        cached_scale_ = GetQuantizeScale(out_type, data_min, data_max);
      }
      mkldnn::primitive_attr attr;
      const int mask            = 0;
      std::vector<float> scales = {cached_scale_};
      attr.set_output_scales(mask, scales);
      if (shifted_output) {
        // TODO(sfraczek): change to zero point when optimized in oneDNN
        dnnl::post_ops po;
        po.append_sum();
        attr.set_post_ops(po);
      }
      mkldnn::engine cpu_engine = mxnet::CpuEngine::Get()->get_engine();
      auto i_desc               = i_mem->get_desc();
      size_t i_ndim             = in_buffer.shape().ndim();
      if (i_ndim == 4) {
        mkldnn::memory::format_tag o_fmt = mkldnn::memory::format_tag::nhwc;
        mkldnn::memory::dims o_dims(i_desc.data.dims, i_desc.data.dims + i_desc.data.ndims);
        o_desc_ = mkldnn::memory::desc(o_dims, get_mkldnn_type(out_type), o_fmt);
      } else {
        o_desc_                = i_desc;
        o_desc_.data.data_type = get_mkldnn_type_t(out_type);
      }
      auto reorder_pd =
          mkldnn::reorder::primitive_desc(cpu_engine, i_desc, cpu_engine, o_desc_, attr);
      fwd_pd_     = std::make_shared<mkldnn::reorder>(reorder_pd);
      initalized_ = true;
    }
    auto o_mem             = CreateMKLDNNMem(outputs[0], o_desc_, req[0]);
    args_[MKLDNN_ARG_FROM] = *i_mem;
    args_[MKLDNN_ARG_TO]   = *o_mem.second;
    MKLDNNStream::Get()->RegisterPrimArgs(*fwd_pd_, args_);
    CommitOutput(outputs[0], o_mem);
    if (shifted_output) {
      uint8_t* raw_out_mem = static_cast<uint8_t*>(o_mem.second->get_data_handle());
      std::fill_n(raw_out_mem, outputs[0].shape().Size(), cached_shift_);
    }
    MKLDNNStream::Get()->Submit();
  }
}

static void SgMKLDNNQuantizeForward(const OpStatePtr& state_ptr,
                                    const OpContext& ctx,
                                    const std::vector<NDArray>& inputs,
                                    const std::vector<OpReqType>& req,
                                    const std::vector<NDArray>& outputs) {
  SgMKLDNNQuantizeOperator& op = state_ptr.get_state<SgMKLDNNQuantizeOperator>();
  op.Forward(ctx, inputs, req, outputs);
}

}  // namespace op
}  // namespace mxnet

#endif  // MXNET_USE_MKLDNN == 1
#endif  // MXNET_OPERATOR_QUANTIZATION_MKLDNN_MKLDNN_QUANTIZE_V2_INL_H_
