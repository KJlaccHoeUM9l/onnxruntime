// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "conv.h"
#include "core/graph/constants.h"
#include "core/graph/graph.h"
#include "core/graph/graph_utils.h"
#include "core/framework/transpose_helper.h"
#include "core/providers/utils.h"
#include "core/providers/xnnpack/detail/utils.h"
#include "core/framework/tensorprotoutils.h"

namespace onnxruntime {
namespace xnnpack {

namespace {
Status CreateXnnpackKernel(const ConvAttributes& conv_attrs,
                           int64_t C, int64_t M,
                           const TensorShapeVector& kernel_shape,
                           const std::optional<std::pair<float, float>>& clip_min_max,
                           const Tensor& W, const Tensor* B_Ts,
                           struct xnn_operator*& p,
#ifdef XNN_CACHE_ENABLE
                           xnn_caches_t caches_t,
#endif
                           QuantParam* quant_param,
                           xnn_compute_type conv_type) {

  uint32_t kernel_height = gsl::narrow<uint32_t>(kernel_shape[0]);
  uint32_t kernel_width = gsl::narrow<uint32_t>(kernel_shape[1]);

  uint32_t input_padding_top = gsl::narrow<uint32_t>(conv_attrs.pads[0]);
  uint32_t input_padding_left = gsl::narrow<uint32_t>(conv_attrs.pads[1]);
  uint32_t input_padding_bottom = gsl::narrow<uint32_t>(conv_attrs.pads[2]);
  uint32_t input_padding_right = gsl::narrow<uint32_t>(conv_attrs.pads[3]);

  uint32_t subsampling_height = gsl::narrow<uint32_t>(conv_attrs.strides[0]);
  uint32_t subsampling_width = gsl::narrow<uint32_t>(conv_attrs.strides[1]);
  uint32_t dilation_height = gsl::narrow<uint32_t>(conv_attrs.dilations[0]);
  uint32_t dilation_width = gsl::narrow<uint32_t>(conv_attrs.dilations[1]);

  uint32_t flags = 0;
  if (conv_attrs.auto_pad == AutoPadType::SAME_UPPER) {
    flags |= XNN_FLAG_TENSORFLOW_SAME_PADDING;
  }

  xnn_status status = xnn_status::xnn_status_uninitialized;
  p = nullptr;

  //with the following IC and OC number, we can cover depthwise and regular conv at the same time
  int32_t group_count = gsl::narrow<uint32_t>(conv_attrs.group);
  int32_t group_input_channels = gsl::narrow<uint32_t>(C / group_count);
  int32_t group_output_channels = gsl::narrow<uint32_t>(M / group_count);
  if(conv_type == xnn_compute_type_fp32) {
    float output_min = clip_min_max ? clip_min_max->first : -INFINITY;
    float output_max = clip_min_max ? clip_min_max->second : INFINITY;
    auto* B_data = B_Ts ? B_Ts->Data<float>() : nullptr;
    status = xnn_create_convolution2d_nhwc_f32(
        input_padding_top, input_padding_right, input_padding_bottom, input_padding_left,
        kernel_height, kernel_width,
        subsampling_height, subsampling_width,
        dilation_height, dilation_width,
        gsl::narrow<uint32_t>(conv_attrs.group),
        group_input_channels, group_output_channels,  // groups, group_input_channels, group_output_channels
        C, M,                                         // input channel stride, output channel stride
        W.Data<float>(), B_data,
        output_min, output_max, flags,
#ifdef XNN_CACHE_ENABLE
        caches_t,
#endif
        &p);
  } else if (conv_type == xnn_compute_type_qs8) {
    int8_t output_min = -126;
    int8_t output_max = 126;
    auto* B_data = B_Ts ? B_Ts->Data<int32_t>() : nullptr;
    status = xnn_create_convolution2d_nhwc_qs8(
        input_padding_top, input_padding_right, input_padding_bottom, input_padding_left,
        kernel_height, kernel_width,
        subsampling_height, subsampling_width,
        dilation_height, dilation_width,
        static_cast<uint32_t>(group_count),
        static_cast<size_t>(group_input_channels),
        static_cast<size_t>(group_output_channels),
        static_cast<size_t>(C),
        static_cast<size_t>(M),
        static_cast<int8_t>(quant_param->X_zero_point_value), quant_param->X_scale_value,
        quant_param->W_scale_value,W.Data<int8_t>(), B_data,
        static_cast<int8_t>(quant_param->Y_zero_point_value), quant_param->Y_scale_value,
        output_min, output_max,
        0,  // flags
#ifdef XNN_CACHE_ENABLE
        caches_t,
#endif
        &p);
  } else if (conv_type == xnn_compute_type_qc8) {
    auto* B_data = B_Ts ? B_Ts->Data<int32_t>() : nullptr;
    int8_t output_min = -126;
    int8_t output_max = 126;
    status = xnn_create_convolution2d_nhwc_qc8(
        input_padding_top, input_padding_right, input_padding_bottom, input_padding_left,
        kernel_height, kernel_width,
        subsampling_height, subsampling_width,
        dilation_height, dilation_width,
        static_cast<uint32_t>(group_count),
        static_cast<size_t>(group_input_channels),
        static_cast<size_t>(group_output_channels),
        static_cast<size_t>(C),
        static_cast<size_t>(M),
        static_cast<int8_t>(quant_param->X_zero_point_value), quant_param->X_scale_value,
        &quant_param->W_scale_value,
        W.Data<int8_t>(), B_data,
        quant_param->Y_zero_point_value, quant_param->Y_scale_value,
        output_min, output_max,
        0,  // flags
#ifdef XNN_CACHE_ENABLE
        caches_t,
#endif
        &p);
  } else if (conv_type == xnn_compute_type_qu8) {
    auto* B_data = B_Ts ? B_Ts->Data<int32_t>() : nullptr;
    uint8_t output_min = clip_min_max ? static_cast<uint8_t>(clip_min_max->first) : 0;
    uint8_t output_max = clip_min_max ? static_cast<uint8_t> (clip_min_max->second) : 255;
    status = xnn_create_convolution2d_nhwc_qu8(
        input_padding_top, input_padding_right, input_padding_bottom, input_padding_left,
        kernel_height, kernel_width,
        subsampling_height, subsampling_width,
        dilation_height, dilation_width,
        static_cast<uint32_t>(group_count),
        static_cast<size_t>(group_input_channels),
        static_cast<size_t>(group_output_channels),
        static_cast<size_t>(C),
        static_cast<size_t>(M),
        quant_param->X_zero_point_value, quant_param->X_scale_value,
        quant_param->W_zero_point_value, quant_param->W_scale_value,
        W.Data<uint8_t>(), B_data,
        quant_param->Y_zero_point_value, quant_param->Y_scale_value,
        output_min, output_max,
        0,  // flags
#ifdef XNN_CACHE_ENABLE
        caches_t,
#endif
        &p);
  } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                             "Failed to create xnnpack kernel. unsupported kernel type  ");
  }
  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                           "Failed to create xnnpack kernel. xnn_create_convolution2d_nhwc_f32 returned ", status);
  }

  return Status::OK();
}

xnn_compute_type ParseQuantParamAndConType(const OpKernelInfo& info, const onnxruntime::Node& node, QuantParam& quant_param_, int32_t x_dtype) {
  // quant param, which used in create xnnpack_conv_kernel
  const onnxruntime::Tensor* X_zero_point = nullptr;
  const onnxruntime::Tensor* W_zero_point = nullptr;
  const onnxruntime::Tensor* Y_zero_point = nullptr;
  ORT_ENFORCE(info.TryGetConstantInput(Conv::InputTensors::IN_X_ZERO_POINT, &X_zero_point),
              "X_zero_point input was not constant initializer. XNNPACK EP should not have asked for the node. Node name:",
              node.Name());
  ORT_ENFORCE(info.TryGetConstantInput(Conv::InputTensors::IN_W_ZERO_POINT, &W_zero_point),
              "W_zero_point input was not constant initializer. XNNPACK EP should not have asked for the node. Node name:",
              node.Name());
  ORT_ENFORCE(info.TryGetConstantInput(Conv::InputTensors::IN_Y_ZERO_POINT, &Y_zero_point),
              "Y_zero_point input was not constant initializer. XNNPACK EP should not have asked for the node. Node name:",
              node.Name());

  quant_param_.X_zero_point_value = *(X_zero_point->template Data<uint8_t>());
  quant_param_.W_zero_point_value = *(W_zero_point->template Data<uint8_t>());
  quant_param_.Y_zero_point_value = *(Y_zero_point->template Data<uint8_t>());

  const onnxruntime::Tensor* X_scale = nullptr;
  const onnxruntime::Tensor* W_scale = nullptr;
  const onnxruntime::Tensor* Y_scale = nullptr;
  ORT_ENFORCE(info.TryGetConstantInput(Conv::InputTensors::IN_X_SCALE, &X_scale),
              "X_scale input was not constant initializer. XNNPACK EP should not have asked for the node. Node name:",
              node.Name());
  ORT_ENFORCE(info.TryGetConstantInput(Conv::InputTensors::IN_W_SCALE, &W_scale),
              "W_scale input was not constant initializer. XNNPACK EP should not have asked for the node. Node name:",
              node.Name());
  ORT_ENFORCE(info.TryGetConstantInput(Conv::InputTensors::IN_Y_SCALE, &Y_scale),
              "Y_scale input was not constant initializer. XNNPACK EP should not have asked for the node. Node name:",
              node.Name());

  quant_param_.X_scale_value = *(X_scale->template Data<float>());
  quant_param_.W_scale_value = *(W_scale->template Data<float>());
  quant_param_.Y_scale_value = *(Y_scale->template Data<float>());
  xnn_compute_type conv_type = xnn_compute_type_invalid;
  if (x_dtype == ONNX_NAMESPACE::TensorProto_DataType_INT8) {
    if (!IsScalarOr1ElementVector(W_scale)) {
      conv_type = xnn_compute_type_qc8;
      quant_param_.W_scale_arr = W_scale->template Data<float>();
    } else {
      conv_type = xnn_compute_type_qs8;
    }
  } else if (x_dtype == ONNX_NAMESPACE::TensorProto_DataType_UINT8) {
    conv_type = xnn_compute_type_qu8;
  }
  return conv_type;
}

}  // namespace


Conv::Conv(const OpKernelInfo& info) : OpKernel(info), conv_attrs_{info} {
  // get values from any fusion with an activation
  if (info.GetAttr<std::string>("activation", &conv_attrs_.activation).IsOK()) {
    std::vector<float> activation_params;

    // min/max could be from Clip or Relu
    if (info.GetAttrs<float>("activation_params", activation_params).IsOK()) {
      if (activation_params.size() == 2) {
        clip_min_max_ = {activation_params[0], activation_params[1]};
      }
    }
  }
  // xnnpack cache_code, this new feature is enabled on the latest XNNPACK
#ifdef XNN_CACHE_ENABLE
#if XNN_PLATFORM_JIT
  xnn_init_code_cache(&code_cache_);
#endif
  caches_.code_cache = &code_cache_;
#endif
  const auto& node{Node()};

  const auto& input_defs = node.InputDefs();
  const NodeArg& X = *input_defs[0];
  C_ = X.Shape()->dim(3).dim_value();  // input is NHWC. op support checker made sure C dim was known

  // as the weight input is a constant initializer we can calculate all the sizes here instead of in Compute
  const Tensor* W = nullptr;

  if (X.TypeAsProto()->tensor_type().elem_type() == ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
    ORT_ENFORCE(info.TryGetConstantInput(1, &W),
                "Weight input was not constant initializer. XNNPACK EP should not have asked for the node. Node name:",
                node.Name());
    conv_type_ = xnn_compute_type_fp32;
  } else {
    ORT_ENFORCE(info.TryGetConstantInput(InputTensors::IN_W, &W),
                "Weight input wasnot constant initializer. XNNPACK EP should not have asked for the node. Node name:",
                node.Name());
    conv_type_ = ParseQuantParamAndConType(info, node, quant_param_, X.TypeAsProto()->tensor_type().elem_type());
  }
  // 'M' is first dim of weight. Prepacking will alter the layout of W later
  M_ = W->Shape()[0];

  // this happens before PrePack, so the W input is still in the ONNX spec format
  ORT_THROW_IF_ERROR(conv_attrs_.ComputeKernelShape(W->Shape(), kernel_shape_));

  if (conv_attrs_.pads.empty()) {
    conv_attrs_.pads.resize(kernel_shape_.size() * 2, 0);
  }

  if (conv_attrs_.dilations.empty()) {
    conv_attrs_.dilations.resize(kernel_shape_.size(), 1);
  }

  if (conv_attrs_.strides.empty()) {
    conv_attrs_.strides.resize(kernel_shape_.size(), 1);
  }

  // we only take nodes with no bias, or a constant bias.
  bool has_bias = input_defs.size() == 3 && input_defs[2]->Exists();
  if (conv_type_ == xnn_compute_type_fp32) {
    ORT_ENFORCE(has_bias == false || info.TryGetConstantInput(2, &B_),
                "Invalid Node with non-constant Bias input. XNNPACK EP should not have asked for the node. Node name:",
                node.Name());
  }else{
    has_bias = input_defs.size() == (InputTensors::IN_BIAS + 1) && input_defs[InputTensors::IN_BIAS]->Exists();
    ORT_ENFORCE(has_bias == false || info.TryGetConstantInput(InputTensors::IN_BIAS, & B_),
                "Invalid Node with non-constant Bias input. XNNPACK EP should not have asked for the node. Node name:",
                node.Name());
  }



  // have to delay creating the xnnpack kernel until after the weights are pre-packed.
}

// use PrePack to handle the weight layout change as that's not a simple NCHW -> NHWC transpose
Status Conv::PrePack(const Tensor& tensor, int input_idx, AllocatorPtr alloc,
                     /*out*/ bool& is_packed,
                     /*out*/ PrePackedWeights* /*prepacked_weights*/) {
  is_packed = false;
  // only layout of weight input is adjusted via PrePack
  if ((conv_type_ == xnn_compute_type_fp32 && input_idx == 1) || 
      (conv_type_ != xnn_compute_type_fp32 && input_idx == InputTensors::IN_W)) {
    // Transpose from {M, C/group, kH, kW} to {M, kH, kW, C/group}
    auto orig_shape = tensor.Shape();

    std::vector<size_t> perm{0, 2, 3, 1};
    std::vector<int64_t> new_dims{orig_shape[0],
                                  orig_shape[2],
                                  orig_shape[3],
                                  orig_shape[1]};

    packed_w_ = Tensor::Create(tensor.DataType(), TensorShape(new_dims), alloc);

    SingleAxisTranspose(perm, tensor, *packed_w_, /*from*/ 1, /*to*/ 3);

    is_packed = true;

    // we can create the kernel now
    struct xnn_operator* p = nullptr;
    auto ret = CreateXnnpackKernel(conv_attrs_, C_, M_, kernel_shape_, clip_min_max_, *packed_w_,
                                   B_, p,
#ifdef XNN_CACHE_ENABLE
                                   &xnn_caches_,
#endif
                                   &quant_param_, conv_type_);
    ORT_RETURN_IF_ERROR(ret);
    op0_.reset(p);
  }

  return Status::OK();
}

Status Conv::Compute(OpKernelContext* context) const {
  const Tensor& X = *context->Input<Tensor>(0);  // this is in NHWC format
  const auto& X_shape = X.Shape();
  const int64_t N = X_shape[0];  // input is NHWC
  const int64_t H = X_shape[1];
  const int64_t W = X_shape[2];

  // We don't need to call ValidateInputShape as we checked validity in ConvChecker.
  // We also can't use ValidateInputShape as-is as the weight tensor was pre-packed and the layout was changed there.
  // ORT_RETURN_IF_ERROR(conv_attrs_.ValidateInputShape(&X, &W));

  // CPU Conv starts with TensorShapeVector Y_dims({N, M}); and passes in X->Shape().Slice(2);
  // We know this is 2D in NHWC format so we need to start with 'N', pass in the H, W, and append M last
  TensorShapeVector Y_dims({N});
  TensorShape input_shape = {H, W};

  ConvAttributes::ConvPadVector pads(conv_attrs_.pads);
  ORT_RETURN_IF_ERROR(conv_attrs_.InferPadsAndOutputShape(input_shape, kernel_shape_,
                                                          conv_attrs_.strides, conv_attrs_.dilations, pads,
                                                          Y_dims));

  Y_dims.push_back(M_);
  Tensor* Y = context->Output(0, TensorShape(Y_dims));

  // Bail out early if one of the dimensions is zero.
  if (Y->Shape().Size() == 0) {
    return Status::OK();
  }

  xnn_status status = xnn_status_invalid_state;
  if (conv_type_ == xnn_compute_type_fp32) {
    status = xnn_setup_convolution2d_nhwc_f32(op0_.get(), N, H, W, X.Data<float>(), Y->MutableData<float>(),
                                              nullptr /*threadpool*/);  // TBD: how to handle threading
  } else if (conv_type_ == xnn_compute_type_qs8) {
    status = xnn_setup_convolution2d_nhwc_qs8(op0_.get(), N, H, W, X.Data<int8_t>(), Y->MutableData<int8_t>(),
                                              nullptr /*threadpool*/);  // TBD: how to handle threading
  } else if (conv_type_ == xnn_compute_type_qu8) {
    status = xnn_setup_convolution2d_nhwc_qu8(op0_.get(), N, H, W, X.Data<uint8_t>(), Y->MutableData<uint8_t>(),
                                              nullptr /*threadpool*/);  // TBD: how to handle threading
  } else if (conv_type_ == xnn_compute_type_qc8) {
    status = xnn_setup_convolution2d_nhwc_qc8(op0_.get(), N, H, W, X.Data<int8_t>(), Y->MutableData<int8_t>(),
                                              nullptr /*threadpool*/);  // TBD: how to handle threading
  } else {
    status = xnn_status_invalid_state;
  }

  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_setup_convolution2d_nhwc_f32 returned ", status);
  }

  status = xnn_run_operator(op0_.get(), nullptr);
  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_run_operator returned ", status);
  }

  return Status::OK();
}

ONNX_OPERATOR_KERNEL_EX(Conv, kMSInternalNHWCDomain, 11, kXnnpackExecutionProvider,
                        KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
                        Conv);
ONNX_OPERATOR_TYPED_KERNEL_EX(
    QLinearConv,
    kMSInternalNHWCDomain,
    10,
    uint8_t,
    kXnnpackExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T1", DataTypeImpl::GetTensorType<uint8_t>())
        .TypeConstraint("T2", {DataTypeImpl::GetTensorType<uint8_t>(), DataTypeImpl::GetTensorType<int8_t>()})
        .TypeConstraint("T3", DataTypeImpl::GetTensorType<uint8_t>()),
    Conv);
ONNX_OPERATOR_TYPED_KERNEL_EX(
    QLinearConv,
    kMSInternalNHWCDomain,
    10,
    int8_t,
    kXnnpackExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T1", DataTypeImpl::GetTensorType<int8_t>())
        .TypeConstraint("T2", DataTypeImpl::GetTensorType<int8_t>())
        .TypeConstraint("T3", DataTypeImpl::GetTensorType<int8_t>()),
    Conv);
}  // namespace xnnpack
}  // namespace onnxruntime
