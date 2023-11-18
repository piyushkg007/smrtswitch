/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/lite/delegates/hexagon/builders/conv_2d_builder.h"

#include <stdint.h>

#include <limits>

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/delegates/hexagon/hexagon_nn/hexagon_nn.h"
#include "tensorflow/lite/kernels/internal/optimized/optimized_ops.h"
#include "tensorflow/lite/kernels/kernel_util.h"

namespace tflite {
namespace delegates {
namespace hexagon {
namespace {

// Dilated Depthwise Convolution performs SpaceToBatchND & BatchToSpaceND before
// and after the op respectively.
// This helper computes the paddings param for SpaceToBatchND and crops param
// for BatchToSpaceND.
//
// Inspired by tf.nn.with_space_to_batch & tf.required_space_to_batch_paddings.
void ComputeSpaceToBatchParams(int input_height, int input_width,
                               int weights_height, int weights_width,
                               const std::vector<int>& dilation_factors_h_w,
                               const TfLitePadding padding_type,
                               std::vector<int>* paddings,
                               std::vector<int>* crops) {
  // Base paddings depend on padding applied to the Depthwise Conv op.
  // 4-element array: {top, bottom, left, right}.
  std::vector<int> base_paddings(4, 0);
  if (padding_type == kTfLitePaddingSame) {
    const int dilated_weights_h =
        dilation_factors_h_w[0] * (weights_height - 1) + 1;
    const int dilated_weights_w =
        dilation_factors_h_w[1] * (weights_width - 1) + 1;
    base_paddings[0] = (dilated_weights_h - 1) / 2;
    base_paddings[1] = dilated_weights_h - 1 - (dilated_weights_h - 1) / 2;
    base_paddings[2] = (dilated_weights_w - 1) / 2;
    base_paddings[3] = dilated_weights_w - 1 - (dilated_weights_w - 1) / 2;
  }

  // paddings represents {pad_top, pad_bottom, pad_left, pad_right}.
  paddings->resize(4, 0);
  // crops represents {crop_top, crop_bottom, crop_left, crop_right}.
  crops->resize(4, 0);

  // Logic for computing paddings & crops follows.
  // Taken from tf.required_space_to_batch_paddings, but without array
  // operations since we only deal with 2 dimensions.
  int pad_start_h = base_paddings[0];
  int pad_start_w = base_paddings[2];
  int orig_pad_end_h = base_paddings[1];
  int orig_pad_end_w = base_paddings[3];
  int full_input_h = input_height + pad_start_h + orig_pad_end_h;
  int full_input_w = input_width + pad_start_w + orig_pad_end_w;
  int pad_end_extra_h =
      (dilation_factors_h_w[0] - full_input_h % dilation_factors_h_w[0]) %
      dilation_factors_h_w[0];
  int pad_end_extra_w =
      (dilation_factors_h_w[1] - full_input_w % dilation_factors_h_w[1]) %
      dilation_factors_h_w[1];
  int pad_end_h = orig_pad_end_h + pad_end_extra_h;
  int pad_end_w = orig_pad_end_w + pad_end_extra_w;

  // Assign values.
  (*paddings)[0] = pad_start_h;
  (*paddings)[1] = pad_end_h;
  (*paddings)[2] = pad_start_w;
  (*paddings)[3] = pad_end_w;
  (*crops)[0] = 0;
  (*crops)[1] = pad_end_extra_h;
  (*crops)[2] = 0;
  (*crops)[3] = pad_end_extra_w;
}

// Computes output dimensions for the SpaceToBatchND op used in the dilated
// Depthwise Conv case.
// space_to_batch_paddings should be in format {top, bottom, left, right}.
// These are computed from the documentation for SpaceToBatchND_8's output.
void PopulateSpaceToBatchOutputDims(
    int input_batch_size, int input_height_size, int input_width_size,
    int input_depth_size, const std::vector<int>& dilation_factors_h_w,
    const std::vector<int>& space_to_batch_paddings,
    std::vector<int>* space_to_batch_output_dims) {
  // Batches.
  space_to_batch_output_dims->push_back(
      input_batch_size * dilation_factors_h_w[0] * dilation_factors_h_w[1]);
  // Height.
  space_to_batch_output_dims->push_back((space_to_batch_paddings[0] +
                                         input_height_size +
                                         space_to_batch_paddings[1]) /
                                        dilation_factors_h_w[0]);
  // Width.
  space_to_batch_output_dims->push_back((space_to_batch_paddings[2] +
                                         input_width_size +
                                         space_to_batch_paddings[3]) /
                                        dilation_factors_h_w[1]);
  // Depth.
  space_to_batch_output_dims->push_back(input_depth_size);
}

}  // namespace

TfLiteStatus Conv2dOpBuilder::PopulateSubGraph(const TfLiteIntArray* inputs,
                                               const TfLiteIntArray* outputs,
                                               TfLiteContext* context) {
  static std::vector<int> dilation_factors_shape = {1, 1, 1, 2};
  static std::vector<int> paddings_shape = {1, 1, 2, 2};

  // Input data tensor.
  const auto& data_tensor = context->tensors[inputs->data[0]];
  int input_batch_size, input_height_size, input_width_size, input_depth_size;
  GetDims(&input_batch_size, &input_height_size, &input_width_size,
          &input_depth_size, data_tensor.dims);
  float data_min = 0;
  float data_max = 0;
  TF_LITE_ENSURE_STATUS(
      ComputeMinAndMaxQuantValues(data_tensor, &data_min, &data_max));
  auto* data_min_const = graph_builder_->AddConstNodeWithData(
      kScalarShape, reinterpret_cast<char*>(&data_min), sizeof(data_min));
  auto* data_max_const = graph_builder_->AddConstNodeWithData(
      kScalarShape, reinterpret_cast<char*>(&data_max), sizeof(data_max));

  // Gather information about the Convolution operations.
  TfLitePadding padding_type = kTfLitePaddingUnknown;
  TfLiteFusedActivation activation = kTfLiteActNone;
  int stride_height = 0;
  int stride_width = 0;
  bool is_dilated_depthwise_conv = false;
  int channel_multiplier = 1;
  if (op_node_.op_type == OP_Supernode_8x8p32to8) {
    const TfLiteConvParams* conv_params =
        reinterpret_cast<const TfLiteConvParams*>(builtin_data_);
    stride_height = conv_params->stride_height;
    stride_width = conv_params->stride_width;
    padding_type = conv_params->padding;
    activation = conv_params->activation;
  } else if (op_node_.op_type == OP_DepthwiseSupernode_8x8p32to8) {
    const TfLiteDepthwiseConvParams* conv_params =
        reinterpret_cast<const TfLiteDepthwiseConvParams*>(builtin_data_);
    stride_height = conv_params->stride_height;
    stride_width = conv_params->stride_width;
    padding_type = conv_params->padding;
    activation = conv_params->activation;
    channel_multiplier = conv_params->depth_multiplier;
    // We only support dilation for DepthwiseConv.
    if (conv_params->dilation_height_factor > 1 ||
        conv_params->dilation_width_factor > 1) {
      is_dilated_depthwise_conv = true;
      dilation_factors_h_w_.push_back(conv_params->dilation_height_factor);
      dilation_factors_h_w_.push_back(conv_params->dilation_width_factor);
    }
  }

  // Weights tensor
  TF_LITE_ENSURE_STATUS(
      InitializeWeightsNodes(inputs, outputs, context, input_depth_size));

  // Stride node.
  static int dummy = 0;
  stride_shape_ = {1, stride_height, stride_width, 1};
  auto* stride_node = graph_builder_->AddConstNodeWithData(
      stride_shape_.data(), (char*)&dummy, sizeof(dummy));

  // Output dimensions.
  int output_batch_size, output_height_size, output_width_size,
      output_depth_size;
  GetDims(&output_batch_size, &output_height_size, &output_width_size,
          &output_depth_size, context->tensors[outputs->data[0]].dims);
  // Output bounds.
  // TODO(b/129276536): Add support for other activations here. Current
  // implementation assumes None/Relu.
  float output_min = 0;
  float output_max = 0;
  TF_LITE_ENSURE_STATUS(ComputeMinAndMaxQuantValues(
      context->tensors[outputs->data[0]], &output_min, &output_max));
  // These denote the bounds fed to Hexagon's Conv mechanism, which will be
  // different from the TFLite tensor bounds if there is a RELU activation.
  float conv_output_min = output_min;
  float conv_output_max = output_max;
  if (activation == kTfLiteActRelu6) {
    conv_output_min = 0;
    conv_output_max = 6;
  } else if (activation == kTfLiteActRelu1) {
    conv_output_min = 0;
    conv_output_max = 1;
  } else if (activation == kTfLiteActRelu) {
    conv_output_min = 0;
  }
  auto* conv_output_min_const = graph_builder_->AddConstNodeWithData(
      kScalarShape, reinterpret_cast<char*>(&conv_output_min),
      sizeof(conv_output_min));
  auto* conv_output_max_const = graph_builder_->AddConstNodeWithData(
      kScalarShape, reinterpret_cast<char*>(&conv_output_max),
      sizeof(conv_output_max));

  // Bias node.
  TF_LITE_ENSURE_STATUS(InitializeBiasNodes(inputs, outputs, context));

  // TODO(b/143759564): Simplify this method when depth_multiplier support needs
  // generalizing.
  if (channel_multiplier > 1 && input_depth_size == 1) {
    // Depthwise Conv with input_depth == 1 & channel_multiplier > 1 is
    // equivalent to Conv.
    SetOpType(OP_Supernode_8x8p32to8);
  } else if (channel_multiplier > 1) {
    TF_LITE_KERNEL_LOG(
        context, "depth_multiplier > 1 not supported with input_depth > 1");
    return kTfLiteError;
  }

  TensorID output_tensor, output_min_tensor, output_max_tensor;
  if (is_dilated_depthwise_conv) {
    // For dilated Depthwise Conv, we convert this node into SpaceToBatchND, and
    // then chain Supernode & BatchToSpaceND after it.
    int input_batch_size, input_height_size, input_width_size, input_depth_size;
    GetDims(&input_batch_size, &input_height_size, &input_width_size,
            &input_depth_size, data_tensor.dims);
    ComputeSpaceToBatchParams(
        input_height_size, input_width_size, weight_shape_[0], weight_shape_[1],
        dilation_factors_h_w_, padding_type, &space_to_batch_paddings_,
        &batch_to_space_crops_);
    auto* dilation_factors_const = graph_builder_->AddConstNodeWithData(
        dilation_factors_shape.data(), (char*)dilation_factors_h_w_.data(),
        dilation_factors_h_w_.size() * sizeof(stride_height));
    auto* paddings_const = graph_builder_->AddConstNodeWithData(
        paddings_shape.data(), (char*)space_to_batch_paddings_.data(),
        space_to_batch_paddings_.size() * sizeof(stride_height));
    auto* crops_const = graph_builder_->AddConstNodeWithData(
        paddings_shape.data(), (char*)batch_to_space_crops_.data(),
        batch_to_space_crops_.size() * sizeof(stride_height));

    // 1. SpaceToBatch.
    SetOpType(OP_SpaceToBatchND_8);
    AddInput(graph_builder_->GetHexagonTensorId(inputs->data[0]));
    AddInput(TensorID(dilation_factors_const->GetID(), 0));
    AddInput(TensorID(paddings_const->GetID(), 0));
    AddInput(TensorID(data_min_const->GetID(), 0));
    AddInput(TensorID(data_max_const->GetID(), 0));
    std::vector<int> space_to_batch_output_dims;
    PopulateSpaceToBatchOutputDims(
        input_batch_size, input_height_size, input_width_size, input_depth_size,
        dilation_factors_h_w_, space_to_batch_paddings_,
        &space_to_batch_output_dims);
    TensorID space_to_batch_op_out =
        AddOutput(sizeof(uint8_t), 4, space_to_batch_output_dims);
    AddOutput(sizeof(float), 4, kScalarShape);
    AddOutput(sizeof(float), 4, kScalarShape);

    // 2. Depthwise Conv.
    auto* conv_op = graph_builder_->AddNode(GetTFLiteNodeID());
    conv_op->SetOpType(OP_DepthwiseSupernode_8x8p32to8);
    conv_op->AddInput(space_to_batch_op_out);
    conv_op->AddInput(TensorID(weights_data_node_->GetID(), 0));
    conv_op->AddInput(TensorID(data_min_const->GetID(), 0));
    conv_op->AddInput(TensorID(data_max_const->GetID(), 0));
    conv_op->AddInput(TensorID(weights_min_node_->GetID(), 0));
    conv_op->AddInput(TensorID(weights_max_node_->GetID(), 0));
    conv_op->AddInput(TensorID(stride_node->GetID(), 0));
    conv_op->AddInput(TensorID(bias_data_node_->GetID(), 0));
    conv_op->AddInput(TensorID(bias_min_node_->GetID(), 0));
    conv_op->AddInput(TensorID(bias_max_node_->GetID(), 0));
    conv_op->AddInput(TensorID(conv_output_min_const->GetID(), 0));
    conv_op->AddInput(TensorID(conv_output_max_const->GetID(), 0));
    if (channel_scales_node_ != nullptr) {
      conv_op->AddInput(TensorID(channel_scales_node_->GetID(), 0));
    }
    // The padding is handled by the SpaceToBatch/BatchToSpace ops surrounding
    // this node. Hence, this op's padding remains VALID only.
    // tf.nn.with_space_to_batch's docs state the following pattern:
    // """
    // batch_to_space_nd(
    //  op(space_to_batch_nd(input, adjusted_dilation_rate, adjusted_paddings),
    //     num_spatial_dims,
    //     "VALID")
    //  adjusted_dilation_rate,
    //  adjusted_crops)
    // """
    conv_op->SetPaddingType(NN_PAD_VALID);
    // These dimensions are probably a little excessive, but they upper-bound
    // the possible output from DepthwiseConv.
    // TODO(b/139955809): Find better bounds?
    TensorID conv_output = conv_op->AddOutput(
        sizeof(uint8_t), 4,
        {output_batch_size * dilation_factors_h_w_[0] *
             dilation_factors_h_w_[1],
         output_height_size, output_width_size, output_depth_size});
    conv_op->AddOutput(sizeof(float), 4, kScalarShape);
    conv_op->AddOutput(sizeof(float), 4, kScalarShape);

    // 3. BatchToSpace.
    auto* batch_to_space_op = graph_builder_->AddNode(GetTFLiteNodeID());
    batch_to_space_op->SetOpType(OP_BatchToSpaceND_8);
    batch_to_space_op->AddInput(conv_output);
    batch_to_space_op->AddInput(TensorID(dilation_factors_const->GetID(), 0));
    batch_to_space_op->AddInput(TensorID(crops_const->GetID(), 0));
    batch_to_space_op->AddInput(TensorID(conv_output_min_const->GetID(), 0));
    batch_to_space_op->AddInput(TensorID(conv_output_max_const->GetID(), 0));
    output_tensor =
        batch_to_space_op->AddOutput(sizeof(uint8_t), 4,
                                     {output_batch_size, output_height_size,
                                      output_width_size, output_depth_size});
    output_min_tensor =
        batch_to_space_op->AddOutput(sizeof(float), 4, kScalarShape);
    output_max_tensor =
        batch_to_space_op->AddOutput(sizeof(float), 4, kScalarShape);
  } else {
    // Standard case.
    // Padding type.
    if (padding_type == kTfLitePaddingSame) {
      SetPaddingType(NN_PAD_SAME);
    } else if (padding_type == kTfLitePaddingValid) {
      SetPaddingType(NN_PAD_VALID);
    }
    // Inputs
    AddInput(graph_builder_->GetHexagonTensorId(inputs->data[0]));
    AddInput(TensorID(weights_data_node_->GetID(), 0));
    AddInput(TensorID(data_min_const->GetID(), 0));
    AddInput(TensorID(data_max_const->GetID(), 0));
    AddInput(TensorID(weights_min_node_->GetID(), 0));
    AddInput(TensorID(weights_max_node_->GetID(), 0));
    AddInput(TensorID(stride_node->GetID(), 0));
    AddInput(TensorID(bias_data_node_->GetID(), 0));
    AddInput(TensorID(bias_min_node_->GetID(), 0));
    AddInput(TensorID(bias_max_node_->GetID(), 0));
    AddInput(TensorID(conv_output_min_const->GetID(), 0));
    AddInput(TensorID(conv_output_max_const->GetID(), 0));
    if (channel_scales_node_ != nullptr) {
      AddInput(TensorID(channel_scales_node_->GetID(), 0));
    }
    // Outputs
    output_tensor = AddOutput(sizeof(uint8_t), 4,
                              {output_batch_size, output_height_size,
                               output_width_size, output_depth_size});
    output_min_tensor = AddOutput(sizeof(float), 4, kScalarShape);
    output_max_tensor = AddOutput(sizeof(float), 4, kScalarShape);
  }

  // Requantize if activation was not None.
  if (activation != kTfLiteActNone) {
    auto* requantized_min_const = graph_builder_->AddConstNodeWithData(
        kScalarShape, reinterpret_cast<char*>(&output_min), sizeof(output_min));
    auto* requantized_max_const = graph_builder_->AddConstNodeWithData(
        kScalarShape, reinterpret_cast<char*>(&output_max), sizeof(output_max));
    auto* requantize_op = graph_builder_->AddNode(GetTFLiteNodeID());
    requantize_op->SetOpType(OP_Requantize_8to8);
    requantize_op->AddInput(output_tensor);
    requantize_op->AddInput(output_min_tensor);
    requantize_op->AddInput(output_max_tensor);
    requantize_op->AddInput(TensorID(requantized_min_const->GetID(), 0));
    requantize_op->AddInput(TensorID(requantized_max_const->GetID(), 0));
    node_output_ =
        requantize_op->AddOutput(sizeof(uint8_t), 4,
                                 {output_batch_size, output_height_size,
                                  output_width_size, output_depth_size});
    requantize_op->AddOutput(sizeof(float), 4, kScalarShape);
    requantize_op->AddOutput(sizeof(float), 4, kScalarShape);
  } else {
    node_output_ = output_tensor;
  }

  return kTfLiteOk;
}

TfLiteStatus Conv2dOpBuilder::RegisterOutputs(const TfLiteIntArray* outputs,
                                              TfLiteContext* context) {
  // Should be only 1 output.
  graph_builder_->AddTensorWithID(outputs->data[0], node_output_.first,
                                  node_output_.second);
  return kTfLiteOk;
}

Conv2dOpBuilder::~Conv2dOpBuilder() {}

OpBuilder* CreateConv2DBuilder(GraphBuilder* graph_builder, int op_type) {
  return new Conv2dOpBuilder(graph_builder, op_type);
}

}  // namespace hexagon
}  // namespace delegates
}  // namespace tflite
