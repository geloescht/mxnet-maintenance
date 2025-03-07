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
 * \file mkldnn_pooling.cc
 * \brief
 * \author Tao Lv
 */

#if MXNET_USE_MKLDNN == 1

#include "./mkldnn_pooling-inl.h"

namespace mxnet {
namespace op {

static inline mkldnn::memory::data_type get_data_type(const mkldnn::memory::desc& md) {
  return static_cast<mkldnn::memory::data_type>(md.data_type());
}

void MKLDNNPoolingFwd::Init(const mxnet::NDArray& input,
                            const mxnet::NDArray& output,
                            const mkldnn::memory::dims& kernel,
                            const mkldnn::memory::dims& strides,
                            const mkldnn::memory::dims& pad_l,
                            const mkldnn::memory::dims& pad_r,
                            const bool is_train,
                            const mkldnn::algorithm alg_kind) {
  const auto src_md = static_cast<const mkldnn::memory*>(input.GetMKLDNNData())->get_desc();
  const auto dst_md = GetMemDesc(output);
  const mkldnn::engine engine = CpuEngine::Get()->get_engine();

  if (alg_kind != mkldnn::algorithm::pooling_max && alg_kind != mkldnn::algorithm::pooling_avg &&
      alg_kind != mkldnn::algorithm::pooling_avg_include_padding &&
      alg_kind != mkldnn::algorithm::pooling_avg_exclude_padding) {
    LOG(FATAL) << "MKLDNN Pooling: algorithm is not supported";
  }

  mkldnn::prop_kind prop = mkldnn::prop_kind::forward_scoring;
  if (is_train && alg_kind != mkldnn::algorithm::pooling_avg) {
    prop = mkldnn::prop_kind::forward_training;
  }
  if (is_train && prop == mkldnn::prop_kind::forward_scoring) {
    LOG(INFO) << "MKLDNN Pooling: training with prop_kind is forward_scoring";
  }

  const auto fwd_desc =
      mkldnn::pooling_forward::desc(prop, alg_kind, src_md, dst_md, strides, kernel, pad_l, pad_r);
  this->fwd_pd_.reset(new mkldnn::pooling_forward::primitive_desc(fwd_desc, engine));
  this->fwd_.reset(new mkldnn::pooling_forward(*(this->fwd_pd_)));

  return;
}

void MKLDNNPoolingFwd::Execute(const NDArray& in_data,
                               const OpReqType req,
                               const NDArray& out_data,
                               const NDArray* workspace,
                               const bool use_adaptive_pooling) {
  NDArray in_buffer = in_data;
  if (in_data.IsView() && in_data.IsMKLDNNData())
    in_buffer = in_data.Reorder2Default();

  auto input_mem     = static_cast<const mkldnn::memory*>(in_buffer.GetMKLDNNData());
  auto output_mem_t_ = CreateMKLDNNMem(out_data, this->fwd_pd_->dst_desc(), req);

  mkldnn_args_map_t args = {
      {MKLDNN_ARG_SRC, *input_mem},
      {MKLDNN_ARG_DST, *(output_mem_t_.second)},
  };

  if (this->with_workspace_ && !use_adaptive_pooling) {
    auto engine = CpuEngine::Get()->get_engine();

    if (workspace == nullptr) {
      LOG(FATAL) << "MKLDNN Pooling: incorrect workspace input";
    }

    auto ws = std::make_shared<mkldnn::memory>(
        (*(this->fwd_pd_)).workspace_desc(),
        engine,
        static_cast<const mkldnn::memory*>(
            static_cast<const mkldnn::memory*>(workspace->GetMKLDNNData()))
            ->get_data_handle());
    args[MKLDNN_ARG_WORKSPACE] = *ws;
  }
  if (this->fwd_) {
    MKLDNNStream::Get()->RegisterPrimArgs(*(this->fwd_), args);
    CommitOutput(out_data, output_mem_t_);
    MKLDNNStream::Get()->Submit();
  } else {
    LOG(FATAL) << "MKLDNN Pooling: forward primitive is nullptr";
  }
}

mkldnn::algorithm GetMKLDNNPoolingAlgorithm(const PoolingParam& param) {
  switch (param.pool_type) {
    case pool_enum::kMaxPooling:
      return mkldnn::algorithm::pooling_max;
    case pool_enum::kAvgPooling:
      if (param.count_include_pad.has_value() && !param.count_include_pad.value()) {
        return mkldnn::algorithm::pooling_avg_exclude_padding;
      } else {
        return mkldnn::algorithm::pooling_avg_include_padding;
      }
    default:
      LOG(FATAL) << "MKLDNN Pooling: Unknown pooling method.";
      return mkldnn::algorithm::pooling_max;
  }
}

void PrepareKernels(mkldnn::memory::dims* kernel,
                    mkldnn::memory::dims* strides,
                    mkldnn::memory::dims* pad_l,
                    mkldnn::memory::dims* pad_r,
                    const PoolingParam& param,
                    const mkldnn::memory::desc& data_md,
                    int kernel_ndims) {
  CHECK_GE(param.pad.ndim(), kernel_ndims);
  CHECK_GE(param.stride.ndim(), kernel_ndims);

  for (int idx = 0; idx < kernel_ndims; ++idx) {
    kernel->at(idx)  = param.kernel[idx];
    pad_l->at(idx)   = param.pad[idx];
    pad_r->at(idx)   = param.pad[idx];
    strides->at(idx) = param.stride[idx];
  }
  if (param.pooling_convention == pool_enum::kFull) {
    for (int idx = 0; idx < kernel_ndims; ++idx) {
      pad_r->at(idx) = GetPaddingSizeFull(data_md.data.dims[idx + 2],
                                          pad_l->at(idx),
                                          pad_r->at(idx),
                                          kernel->at(idx),
                                          strides->at(idx));
    }
  }
  if (param.global_pool) {
    for (int idx = 0; idx < kernel_ndims; ++idx) {
      kernel->at(idx)  = data_md.data.dims[idx + 2];
      strides->at(idx) = 1;
      pad_l->at(idx) = pad_r->at(idx) = 0;
    }
  }
  for (int idx = 0; idx < kernel_ndims; ++idx) {
    CHECK_GT(kernel->at(idx), 0) << "Filter dimensions cannot be zero.";
  }
}

void InitPoolingPrimitiveParams(const PoolingParam& param,
                                const mkldnn::memory::desc& data_md,
                                const mkldnn::memory::dims& new_kernel,
                                const mkldnn::memory::dims& new_strides,
                                const mkldnn::memory::dims& new_pad_l,
                                const mkldnn::memory::dims& new_pad_r) {
  const int kernel_ndims        = param.kernel.ndim();
  mkldnn::memory::dims& kernel  = const_cast<mkldnn::memory::dims&>(new_kernel);
  mkldnn::memory::dims& strides = const_cast<mkldnn::memory::dims&>(new_strides);
  mkldnn::memory::dims& pad_l   = const_cast<mkldnn::memory::dims&>(new_pad_l);
  mkldnn::memory::dims& pad_r   = const_cast<mkldnn::memory::dims&>(new_pad_r);

  PrepareKernels(&kernel, &strides, &pad_l, &pad_r, param, data_md, kernel_ndims);

  if (pad_l[0] != 0 || (kernel_ndims == 2 && pad_l[1] != 0) ||
      (kernel_ndims == 3 && pad_l[2] != 0)) {
    CHECK(param.pool_type == pool_enum::kAvgPooling || param.pool_type == pool_enum::kMaxPooling)
        << "Padding implemented only for average and max pooling.";
    CHECK_LT(pad_l[0], kernel[0]);
    if (kernel_ndims > 1)
      CHECK_LT(pad_l[1], kernel[1]);
    if (kernel_ndims > 2)
      CHECK_LT(pad_l[2], kernel[2]);
  }
}

mkldnn::pooling_forward::primitive_desc GetPoolingFwdPdesc(const PoolingParam& param,
                                                           const bool is_train,
                                                           const mkldnn::memory::desc& data_md,
                                                           const mkldnn::memory::desc& out_md,
                                                           const bool use_adaptive_pooling) {
  CHECK(param.kernel.ndim() == 1 || param.kernel.ndim() == 2 || param.kernel.ndim() == 3)
      << "Not Implemented";

  const int kernel_ndims =
      use_adaptive_pooling ? mxnet::TShape(data_md.dims()).ndim() : param.kernel.ndim();
  mkldnn::memory::dims kernel(kernel_ndims);
  mkldnn::memory::dims strides(kernel_ndims);
  mkldnn::memory::dims pad_l(kernel_ndims);
  mkldnn::memory::dims pad_r(kernel_ndims);

  const mxnet::TShape input_shape = mxnet::TShape(data_md.dims());
  const mxnet::TShape output_shape = mxnet::TShape(out_md.dims());

  if (use_adaptive_pooling) {
    UseAdaptivePaddingKernel(&kernel, &strides, &pad_l, &pad_r, input_shape, output_shape);
    mkldnn::memory::validate_dims(kernel);
    mkldnn::memory::validate_dims(strides);
    mkldnn::memory::validate_dims(pad_l);
    mkldnn::memory::validate_dims(pad_r);
  } else {
    InitPoolingPrimitiveParams(param, data_md, kernel, strides, pad_l, pad_r);
  }

  const mkldnn::algorithm alg = GetMKLDNNPoolingAlgorithm(param);
  mkldnn::prop_kind kind      = mkldnn::prop_kind::forward_scoring;
  if (is_train && alg != mkldnn::algorithm::pooling_avg) {
    kind = mkldnn::prop_kind::forward_training;
  }

  const mkldnn::pooling_forward::desc poolingFwd_desc(
      kind, alg, data_md, out_md, strides, kernel, pad_l, pad_r);
  return mkldnn::pooling_forward::primitive_desc(poolingFwd_desc, CpuEngine::Get()->get_engine());
}

MKLDNNPoolingFwd& GetPoolingFwd(const PoolingParam& param,
                                const bool is_train,
                                const NDArray& data,
                                const NDArray& output,
                                const bool use_adaptive_pooling) {
#if DMLC_CXX11_THREAD_LOCAL
  static thread_local std::unordered_map<MKLDNNPoolingSignature, MKLDNNPoolingFwd, OpHash>
      pooling_fwds;
#else
  static MX_THREAD_LOCAL std::unordered_map<MKLDNNPoolingSignature, MKLDNNPoolingFwd, OpHash>
      pooling_fwds;
#endif

  const bool with_workspace = is_train && MKLDNNRequireWorkspace(param);
  MKLDNNPoolingSignature key(param);
  key.AddSign(is_train);
  key.AddSign(with_workspace);
  key.AddSign(data);
  key.AddSign(output);
  if (use_adaptive_pooling) {
    key.AddSign(use_adaptive_pooling);
  }
  auto it = pooling_fwds.find(key);
  if (it == pooling_fwds.end()) {
    CHECK(use_adaptive_pooling || (param.kernel.ndim() >= 1 && param.kernel.ndim() <= 3))
        << "Not Implemented";
    auto data_md = static_cast<const mkldnn::memory*>(data.GetMKLDNNData())->get_desc();

    const auto kernel_ndims = use_adaptive_pooling ? data.shape().ndim() : param.kernel.ndim();

    mkldnn::memory::dims kernel(kernel_ndims);
    mkldnn::memory::dims strides(kernel_ndims);
    mkldnn::memory::dims pad_l(kernel_ndims);
    mkldnn::memory::dims pad_r(kernel_ndims);

    if (use_adaptive_pooling) {
      UseAdaptivePaddingKernel(&kernel, &strides, &pad_l, &pad_r, data.shape(), output.shape());
      mkldnn::memory::validate_dims(kernel);
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(pad_l);
      mkldnn::memory::validate_dims(pad_r);
    } else {
      InitPoolingPrimitiveParams(param, data_md, kernel, strides, pad_l, pad_r);
    }

    const mkldnn::algorithm alg =
        use_adaptive_pooling ? mkldnn::algorithm::pooling_avg : GetMKLDNNPoolingAlgorithm(param);

    MKLDNNPoolingFwd fwd(
        data, output, kernel, strides, pad_l, pad_r, alg, with_workspace, is_train);
    it = AddToCache(&pooling_fwds, key, fwd);
  }
  return it->second;
}

MKLDNNPoolingBwd::MKLDNNPoolingBwd(const mkldnn::pooling_backward::primitive_desc& pdesc,
                                   bool with_ws)
    : with_workspace(with_ws), pd(pdesc) {
  bwd = std::make_shared<mkldnn::pooling_backward>(pd);
}

const mkldnn::pooling_backward& MKLDNNPoolingBwd::GetBwd() {
  return *this->bwd;
}

MKLDNNPoolingBwd& GetPoolingBwd(const PoolingParam& param,
                                const NDArray& in_data,
                                const NDArray& in_grad,
                                const NDArray& out_grad,
                                const bool use_adaptive_pooling) {
#if DMLC_CXX11_THREAD_LOCAL
  static thread_local std::unordered_map<MKLDNNPoolingSignature, MKLDNNPoolingBwd, OpHash>
      pooling_bwds;
#else
  static MX_THREAD_LOCAL std::unordered_map<MKLDNNPoolingSignature, MKLDNNPoolingBwd, OpHash>
      pooling_bwds;
#endif

  const bool with_workspace = MKLDNNRequireWorkspace(param);
  MKLDNNPoolingSignature key(param);
  key.AddSign(in_data);
  key.AddSign(in_grad);
  key.AddSign(out_grad);
  if (use_adaptive_pooling) {
    key.AddSign(use_adaptive_pooling);
  }

  auto it = pooling_bwds.find(key);
  if (it == pooling_bwds.end()) {
    auto input_mem = static_cast<const mkldnn::memory*>(in_data.GetMKLDNNData());
    const mkldnn::memory::desc data_md = input_mem->get_desc();

    auto dst_dims = mkldnn::memory::dims(out_grad.shape().begin(), out_grad.shape().end());
    auto any      = mkldnn::memory::format_tag::any;
    auto dst_md   = mkldnn::memory::desc(dst_dims, get_data_type(data_md), any);

    // fwd hint
    auto fwd_pd = GetPoolingFwdPdesc(param, true, data_md, dst_md, use_adaptive_pooling);

    // create bwd desc
    auto diff_src_dims = mkldnn::memory::dims(in_grad.shape().begin(), in_grad.shape().end());
    auto diff_src_md   = mkldnn::memory::desc(diff_src_dims, get_data_type(data_md), any);
    auto cpu_engine    = CpuEngine::Get()->get_engine();
    auto alg = use_adaptive_pooling ? mkldnn::algorithm::pooling_avg
                                    : GetMKLDNNPoolingAlgorithm(param);

    const int kernel_ndims = use_adaptive_pooling ? in_grad.shape().ndim() : param.kernel.ndim();
    mkldnn::memory::dims kernel(kernel_ndims);
    mkldnn::memory::dims strides(kernel_ndims);
    mkldnn::memory::dims pad_l(kernel_ndims);
    mkldnn::memory::dims pad_r(kernel_ndims);

    if (use_adaptive_pooling) {
      UseAdaptivePaddingKernel(
          &kernel, &strides, &pad_l, &pad_r, in_grad.shape(), out_grad.shape());
      mkldnn::memory::validate_dims(kernel);
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(pad_l);
      mkldnn::memory::validate_dims(pad_r);
    } else {
      InitPoolingPrimitiveParams(param, data_md, kernel, strides, pad_l, pad_r);
    }

    // use dst_md as diff_dst_md with any format
    auto bwd_desc =
        mkldnn::pooling_backward::desc(alg, diff_src_md, dst_md, strides, kernel, pad_l, pad_r);
    auto pdesc = mkldnn::pooling_backward::primitive_desc(bwd_desc, cpu_engine, fwd_pd);

    MKLDNNPoolingBwd bwd(pdesc, with_workspace);
    it = AddToCache(&pooling_bwds, key, bwd);
  }
  return it->second;
}

void MKLDNNPoolingGradCompute(const nnvm::NodeAttrs &attrs,
                              const OpContext &ctx,
                              const std::vector<NDArray> &inputs,
                              const std::vector<OpReqType> &req,
                              const std::vector<NDArray> &outputs) {
  if (req[0] == kNullOp) {
    return;
  }

  const PoolingParam &param = nnvm::get<PoolingParam>(attrs.parsed);

  const NDArray &out_grad = inputs[0];
  const NDArray *workspace = nullptr;
  const NDArray *in_data = nullptr;
  if (MKLDNNRequireWorkspace(param)) {
    // The first two elements are the gradients of the outputs in forward.
    // The third is the input of forward.
    // The fourth and the fifth are the outputs of forward.
    CHECK_EQ(inputs.size(), 5U);
    in_data = &inputs[2];
    workspace = &inputs[4];
  } else if (!param.IsAdaptivePooling()) {
    CHECK_EQ(inputs.size(), 3U);
    in_data = &inputs[1];
  } else {
    in_data = &inputs[0];
  }
  const NDArray &in_grad = outputs[0];

  TmpMemMgr::Get()->Init(ctx.requested[0]);


  auto &bwd = GetPoolingBwd(param, *in_data, in_grad, out_grad,
                            param.IsAdaptivePooling());
  auto bwd_diff_dst_desc = bwd.pd.diff_dst_desc();
  auto diff_dst_mem =
      static_cast<const mkldnn::memory*>(out_grad.GetMKLDNNDataReorder(&bwd_diff_dst_desc));
  auto diff_src_mem = CreateMKLDNNMem(in_grad, bwd.pd.diff_src_desc(), req[0]);
  mkldnn_args_map_t args = {
      {MKLDNN_ARG_DIFF_DST, *diff_dst_mem},
      {MKLDNN_ARG_DIFF_SRC, *diff_src_mem.second},
  };
  if (MKLDNNRequireWorkspace(param) && workspace != nullptr) {
    args[MKLDNN_ARG_WORKSPACE] = *(static_cast<const mkldnn::memory*>(workspace->GetMKLDNNData()));
  }

  MKLDNNStream::Get()->RegisterPrimArgs(bwd.GetBwd(), args);
  CommitOutput(in_grad, diff_src_mem);
  MKLDNNStream::Get()->Submit();
}

void MKLDNNPoolingCompute(const nnvm::NodeAttrs &attrs, const OpContext &ctx,
                        const std::vector<NDArray> &in_data,
                        const std::vector<OpReqType> &req,
                        const std::vector<NDArray> &out_data) {
  const PoolingParam &param = nnvm::get<PoolingParam>(attrs.parsed);
  const NDArray *workspace = nullptr;
  const bool is_adaptive_pooling = param.IsAdaptivePooling();
  if (MKLDNNRequireWorkspace(param) && !is_adaptive_pooling) {
    CHECK_GT(out_data.size(), 1U);
    workspace = &out_data[1];
  }
  auto &fwd = GetPoolingFwd(param, ctx.is_train, in_data[0], out_data[0],
                            is_adaptive_pooling);
  fwd.Execute(in_data[0], req[0], out_data[0], workspace, is_adaptive_pooling);
}

}  // namespace op
}  // namespace mxnet
#endif  // MXNET_USE_MKLDNN == 1
