#include "tflite_schema.h"
#include <importer/importer.h>
#include <ir/op_utils.h>
#include <ir/ops/binary.h>
#include <ir/ops/concat.h>
#include <ir/ops/constant.h>
#include <ir/ops/conv2d.h>
#include <ir/ops/matmul.h>
#include <ir/ops/pad.h>
#include <ir/ops/reduce.h>
#include <ir/ops/reduce_window2d.h>
#include <ir/ops/reshape.h>
#include <ir/ops/resize_image.h>
#include <ir/ops/transpose.h>
#include <ir/ops/unary.h>
#include <limits>
#include <unordered_map>
#include <xtensor/xadapt.hpp>
#include <xtensor/xarray.hpp>
#include <xtensor/xtensor.hpp>

using namespace nncase;
using namespace nncase::importer;
using namespace nncase::ir;
using namespace flatbuffers;

class tflite_importer
{
public:
    tflite_importer(xtl::span<const uint8_t> model, graph &graph)
        : model_(tflite::GetModel(model.data())), subGraph_(model_->subgraphs()->Get(0)), graph_(graph)
    {
        flatbuffers::Verifier verifier(model.data(), model.size());
        if (!tflite::VerifyModelBuffer(verifier))
            throw std::runtime_error("Invalid tflite model");
    }

    void import()
    {
        auto &operators = *subGraph_->operators();
        for (auto &&op : operators)
            convert_op(*op);

        // connect tensors
        for (auto &&in : input_tensors_)
        {
            auto out_it = output_tensors_.find(in.second);
            if (out_it != output_tensors_.end())
            {
                in.first->connect(*out_it->second);
            }
            else
            {
                auto &tensor = *subGraph_->tensors()->Get(in.second);
                auto &buffer = *model_->buffers()->Get(tensor.buffer());
                auto data = buffer.data();

                if (data)
                {
                    auto type = to_data_type(tensor.type());
                    auto shape = get_shape(*tensor.shape());
                    auto con = graph_.emplace<constant>(type, shape, std::vector<uint8_t>(data->begin(), data->end()));
                    output_tensors_.emplace(in.second, &con->output());
                    in.first->connect(con->output());
                }
            }
        }

        // inputs
        for (auto &&in : input_tensors_)
        {
            if (!in.first->connection())
            {
                // image
                if (in.first->shape().size() == 4)
                {
                    auto node = graph_.emplace<input_node>(in.first->type(), nhwc_to_nchw(in.first->shape()));
                    auto sur_trans = nchw_to_nhwc(node->output().type(), node->output().shape());
                    sur_trans->input().connect(node->output());
                    in.first->connect(sur_trans->output());
                }
                else
                {
                    auto node = graph_.emplace<input_node>(in.first->type(), in.first->shape());
                    in.first->connect(node->output());
                }
            }
        }

        // outputs
        for (auto &&out : output_tensors_)
        {
            if (out.second->connections().empty())
            {
                // image
                if (out.second->shape().size() == 4)
                {
                    auto pre_trans = nhwc_to_nchw(out.second->type(), out.second->shape());
                    auto node = graph_.emplace<output_node>(pre_trans->output().type(), pre_trans->output().shape());
                    pre_trans->input().connect(*out.second);
                    node->input().connect(pre_trans->output());
                }
                else
                {
                    auto node = graph_.emplace<output_node>(out.second->type(), out.second->shape());
                    out.second->connect(node->input());
                }
            }
        }
    }

private:
    void convert_op(const tflite::Operator &op)
    {
        auto opcode = model_->operator_codes()->Get(op.opcode_index());
        auto builtin_code = opcode->builtin_code();
        std::unordered_map<tflite::BuiltinOperator, std::function<void(const tflite::Operator &)>> converters {
            { tflite::BuiltinOperator_ADD, [&](const tflite::Operator &op) { return convert_binary(op, binary_add, op.builtin_options_as_AddOptions()->fused_activation_function()); } },
            { tflite::BuiltinOperator_AVERAGE_POOL_2D, [&](const tflite::Operator &op) { return convert_pool_2d(op, reduce_mean, 0.f); } },
            { tflite::BuiltinOperator_CONCATENATION, [&](const tflite::Operator &op) { return convert_concatenation(op); } },
            { tflite::BuiltinOperator_CONV_2D, [&](const tflite::Operator &op) { return convert_conv_2d(op); } },
            { tflite::BuiltinOperator_DEPTHWISE_CONV_2D, [&](const tflite::Operator &op) { return convert_depthwise_conv_2d(op); } },
            { tflite::BuiltinOperator_DIV, [&](const tflite::Operator &op) { return convert_binary(op, binary_div, op.builtin_options_as_DivOptions()->fused_activation_function()); } },
            { tflite::BuiltinOperator_EXP, [&](const tflite::Operator &op) { return convert_unary(op, unary_exp); } },
            { tflite::BuiltinOperator_FLOOR, [&](const tflite::Operator &op) { return convert_unary(op, unary_floor); } },
            { tflite::BuiltinOperator_FULLY_CONNECTED, [&](const tflite::Operator &op) { return convert_fully_connected(op); } },
            { tflite::BuiltinOperator_MAX_POOL_2D, [&](const tflite::Operator &op) { return convert_pool_2d(op, reduce_max, std::numeric_limits<float>::lowest()); } },
            { tflite::BuiltinOperator_MAXIMUM, [&](const tflite::Operator &op) { return convert_binary(op, binary_max, tflite::ActivationFunctionType_NONE); } },
            { tflite::BuiltinOperator_MEAN, [&](const tflite::Operator &op) { return convert_reduce(op, reduce_mean, 0.0f); } },
            { tflite::BuiltinOperator_MINIMUM, [&](const tflite::Operator &op) { return convert_binary(op, binary_min, tflite::ActivationFunctionType_NONE); } },
            { tflite::BuiltinOperator_MUL, [&](const tflite::Operator &op) { return convert_binary(op, binary_mul, op.builtin_options_as_MulOptions()->fused_activation_function()); } },
            { tflite::BuiltinOperator_LOG, [&](const tflite::Operator &op) { return convert_unary(op, unary_log); } },
            { tflite::BuiltinOperator_NEG, [&](const tflite::Operator &op) { return convert_unary(op, unary_neg); } },
            { tflite::BuiltinOperator_PAD, [&](const tflite::Operator &op) { return convert_pad(op); } },
            { tflite::BuiltinOperator_REDUCE_MAX, [&](const tflite::Operator &op) { return convert_binary(op, binary_max, tflite::ActivationFunctionType_NONE); } },
            { tflite::BuiltinOperator_RESHAPE, [&](const tflite::Operator &op) { return convert_reshape(op); } },
            { tflite::BuiltinOperator_RESIZE_BILINEAR, [&](const tflite::Operator &op) { return convert_resize_image(op, image_resize_bilinear); } },
            { tflite::BuiltinOperator_RSQRT, [&](const tflite::Operator &op) { return convert_unary(op, unary_rsqrt); } },
            { tflite::BuiltinOperator_SIN, [&](const tflite::Operator &op) { return convert_unary(op, unary_sin); } },
            { tflite::BuiltinOperator_SOFTMAX, [&](const tflite::Operator &op) { return convert_softmax(op); } },
        };

        auto it = converters.find(builtin_code);
        if (it == converters.end())
            throw std::runtime_error(std::string("Not supported tflite opcode: ") + tflite::EnumNameBuiltinOperator(builtin_code));

        it->second(op);
    }

    void convert_binary(const tflite::Operator &op, binary_op_t binary_op, tflite::ActivationFunctionType activation)
    {
        auto &input_a = get_tensor(op.inputs(), 0);
        auto &input_b = get_tensor(op.inputs(), 1);

        auto add = graph_.emplace<binary>(binary_add, get_shape(*input_a.shape()), get_shape(*input_b.shape()), to_float_clamp_range(activation));

        input_tensors_.emplace(&add->input_a(), op.inputs()->Get(0));
        input_tensors_.emplace(&add->input_b(), op.inputs()->Get(1));
        output_tensors_.emplace(op.outputs()->Get(0), &add->output());
    }

    void convert_concatenation(const tflite::Operator &op)
    {
        std::vector<shape_t> inputs_shape;
        auto &options = *op.builtin_options_as_ConcatenationOptions();

        for (auto &&in : *op.inputs())
        {
            auto &tensor = *subGraph_->tensors()->Get(in);
            inputs_shape.emplace_back(get_shape(*tensor.shape()));
        }

        auto con = graph_.emplace<concat>(dt_float32, inputs_shape, options.axis());

        for (size_t i = 0; i < op.inputs()->size(); i++)
            input_tensors_.emplace(&con->input_at(i), op.inputs()->Get(i));

        output_tensors_.emplace(op.outputs()->Get(0), &con->output());
    }

    void convert_conv_2d(const tflite::Operator &op)
    {
        auto &input = get_tensor(op.inputs(), 0);
        auto &weights = get_tensor(op.inputs(), 1);
        auto &bias = get_tensor(op.inputs(), 2);
        auto &options = *op.builtin_options_as_Conv2DOptions();

        auto weights_tensor = xt::transpose(load_tensor<float, 4>(weights), { 0, 3, 1, 2 });
        auto bias_tensor = load_tensor<float, 1>(bias);

        auto pre_trans = nhwc_to_nchw(dt_float32, get_shape(*input.shape()));

        auto in_h = pre_trans->output().shape()[2];
        auto in_w = pre_trans->output().shape()[3];
        auto f_h = weights_tensor.shape()[2];
        auto f_w = weights_tensor.shape()[3];
        auto stride_h = options.stride_h();
        auto stride_w = options.stride_w();
        auto dilation_h = options.dilation_h_factor();
        auto dilation_w = options.dilation_w_factor();
        auto pad_h = get_windowed_padding(in_h, f_h, stride_h, dilation_h, options.padding() == tflite::Padding_SAME);
        auto pad_w = get_windowed_padding(in_w, f_w, stride_w, dilation_w, options.padding() == tflite::Padding_SAME);
        auto conv = graph_.emplace<conv2d>(pre_trans->output().shape(), std::move(weights_tensor), std::move(bias_tensor), 1,
            pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w, to_float_clamp_range(options.fused_activation_function()));
        conv->input().connect(pre_trans->output());

        auto sur_trans = nchw_to_nhwc(dt_float32, conv->output().shape());
        sur_trans->input().connect(conv->output());

        input_tensors_.emplace(&pre_trans->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &sur_trans->output());
    }

    void convert_depthwise_conv_2d(const tflite::Operator &op)
    {
        auto &input = get_tensor(op.inputs(), 0);
        auto &weights = get_tensor(op.inputs(), 1);
        auto &bias = get_tensor(op.inputs(), 2);
        auto &options = *op.builtin_options_as_DepthwiseConv2DOptions();

        auto weights_tensor = xt::transpose(load_tensor<float, 4>(weights), { 3, 0, 1, 2 });
        auto bias_tensor = load_tensor<float, 1>(bias);

        auto pre_trans = nhwc_to_nchw(dt_float32, get_shape(*input.shape()));

        auto in_h = pre_trans->output().shape()[2];
        auto in_w = pre_trans->output().shape()[3];
        auto groups = weights_tensor.shape()[0];
        auto f_h = weights_tensor.shape()[2];
        auto f_w = weights_tensor.shape()[3];
        auto stride_h = options.stride_h();
        auto stride_w = options.stride_w();
        auto dilation_h = options.dilation_h_factor();
        auto dilation_w = options.dilation_w_factor();
        auto pad_h = get_windowed_padding(in_h, f_h, stride_h, dilation_h, options.padding() == tflite::Padding_SAME);
        auto pad_w = get_windowed_padding(in_w, f_w, stride_w, dilation_w, options.padding() == tflite::Padding_SAME);
        auto conv = graph_.emplace<conv2d>(pre_trans->output().shape(), std::move(weights_tensor), std::move(bias_tensor), groups,
            pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w, to_float_clamp_range(options.fused_activation_function()));
        conv->input().connect(pre_trans->output());

        auto sur_trans = nchw_to_nhwc(dt_float32, conv->output().shape());
        sur_trans->input().connect(conv->output());

        input_tensors_.emplace(&pre_trans->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &sur_trans->output());
    }

    void convert_fully_connected(const tflite::Operator &op)
    {
        auto &input_a = get_tensor(op.inputs(), 0);
        auto &input_b = get_tensor(op.inputs(), 1);
        auto &bias = get_tensor(op.inputs(), 2);
        auto &options = *op.builtin_options_as_FullyConnectedOptions();

        assert(options.weights_format() == tflite::FullyConnectedOptionsWeightsFormat_DEFAULT);

        auto bias_tensor = load_tensor<float, 1>(bias);

        auto input_b_trans = graph_.emplace<transpose>(dt_float32, get_shape(*input_b.shape()), shape_t { 1, 0 });
        auto fc = graph_.emplace<matmul>(get_shape(*input_a.shape()), input_b_trans->output().shape(), std::move(bias_tensor),
            to_float_clamp_range(options.fused_activation_function()));
        fc->input_b().connect(input_b_trans->output());

        input_tensors_.emplace(&fc->input_a(), op.inputs()->Get(0));
        input_tensors_.emplace(&input_b_trans->input(), op.inputs()->Get(1));
        output_tensors_.emplace(op.outputs()->Get(0), &fc->output());
    }

    void convert_pad(const tflite::Operator &op)
    {
        auto &input = get_tensor(op.inputs(), 0);
        auto &paddings = load_tensor<int32_t, 2>(get_tensor(op.inputs(), 1));
        auto &options = *op.builtin_options_as_PadOptions();

        xt::svector<padding> new_paddings;
        for (size_t i = 0; i < paddings.shape()[0]; i++)
            new_paddings.push_back(padding { paddings[i, 0], paddings[i, 1] });

        auto node = graph_.emplace<pad>(dt_float32, get_shape(*input.shape()), new_paddings, 0.f);

        input_tensors_.emplace(&node->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &node->output());
    }

    void convert_pool_2d(const tflite::Operator &op, reduce_op_t reduce_op, float init_value)
    {
        auto &input = get_tensor(op.inputs(), 0);
        auto &options = *op.builtin_options_as_Pool2DOptions();

        auto pre_trans = nhwc_to_nchw(dt_float32, get_shape(*input.shape()));

        auto in_h = pre_trans->output().shape()[2];
        auto in_w = pre_trans->output().shape()[3];
        auto f_h = options.filter_height();
        auto f_w = options.filter_width();
        auto stride_h = options.stride_h();
        auto stride_w = options.stride_w();
        auto dilation_h = 1;
        auto dilation_w = 1;
        auto pad_h = get_windowed_padding(in_h, f_h, stride_h, dilation_h, options.padding() == tflite::Padding_SAME);
        auto pad_w = get_windowed_padding(in_w, f_w, stride_w, dilation_w, options.padding() == tflite::Padding_SAME);
        auto conv = graph_.emplace<reduce_window2d>(reduce_op, pre_trans->output().shape(), init_value, f_h, f_w, pad_h, pad_w,
            stride_h, stride_w, dilation_h, dilation_w, to_float_clamp_range(options.fused_activation_function()));
        conv->input().connect(pre_trans->output());

        auto sur_trans = nchw_to_nhwc(dt_float32, conv->output().shape());
        sur_trans->input().connect(conv->output());

        input_tensors_.emplace(&pre_trans->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &sur_trans->output());
    }

    void convert_reduce(const tflite::Operator &op, reduce_op_t reduce_op, float init_value)
    {
        auto &input = get_tensor(op.inputs(), 0);
        auto axis = load_tensor<int32_t, 1>(get_tensor(op.inputs(), 1));
        auto &options = *op.builtin_options_as_ReducerOptions();

        auto node = graph_.emplace<reduce>(reduce_op, get_shape(*input.shape()), std::move(axis), init_value, options.keep_dims());

        input_tensors_.emplace(&node->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &node->output());
    }

    void convert_reshape(const tflite::Operator &op)
    {
        auto &input = get_tensor(op.inputs(), 0);
        auto &options = *op.builtin_options_as_ReshapeOptions();

        auto node = graph_.emplace<reshape>(to_data_type(input.type()), get_shape(*input.shape()), get_shape(*options.new_shape()));

        input_tensors_.emplace(&node->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &node->output());
    }

    void convert_resize_image(const tflite::Operator &op, image_resize_mode_t mode)
    {
        auto &input = get_tensor(op.inputs(), 0);
        auto &options = *op.builtin_options_as_ResizeBilinearOptions();
        auto new_size_tensor = load_tensor<int32_t, 1>(get_tensor(op.inputs(), 1));
        std::array<int32_t, 2> new_size { new_size_tensor[0], new_size_tensor[1] };

        auto pre_trans = nhwc_to_nchw(dt_float32, get_shape(*input.shape()));
        auto node = graph_.emplace<resize_image>(to_data_type(input.type()), mode, pre_trans->output().shape(), new_size, options.align_corners());
        auto sur_trans = nchw_to_nhwc(dt_float32, node->output().shape());
        node->input().connect(pre_trans->output());
        sur_trans->input().connect(node->output());

        input_tensors_.emplace(&pre_trans->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &sur_trans->output());
    }

    void convert_softmax(const tflite::Operator &op)
    {
        auto &input = get_tensor(op.inputs(), 0);
        auto &options = *op.builtin_options_as_SoftmaxOptions();

        auto in_shape = get_shape(*input.shape());
        xt::svector<int32_t> reduce_axis;
        if (in_shape.size() == 1)
        {
            reduce_axis.push_back(0);
        }
        else
        {
            for (size_t i = 1; i < in_shape.size(); i++)
                reduce_axis.push_back(i);
        }

        //auto max = graph_.emplace<reduce>(reduce_max, in_shape)
        auto sm = graph_.emplace<softmax>(get_shape(*input.shape()), options.beta());

        input_tensors_.emplace(&sm->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &sm->output());
    }

    void convert_unary(const tflite::Operator &op, unary_op_t unary_op)
    {
        auto &input = get_tensor(op.inputs(), 0);

        auto node = graph_.emplace<unary>(unary_op, get_shape(*input.shape()));

        input_tensors_.emplace(&node->input(), op.inputs()->Get(0));
        output_tensors_.emplace(op.outputs()->Get(0), &node->output());
    }

    const tflite::Tensor &get_tensor(const flatbuffers::Vector<int32_t> *ids, int32_t offset)
    {
        return *subGraph_->tensors()->Get(ids->Get(offset));
    }

    template <class T>
    const tflite::Buffer &get_buffer(const tflite::Tensor &tensor)
    {
        auto expect_type = to_tensor_type<T>();
        auto actual_type = tensor.type();
        if (actual_type != expect_type)
            throw std::runtime_error(std::string("Expect ") + tflite::EnumNameTensorType(expect_type) + " tensor but got " + tflite::EnumNameTensorType(actual_type));

        auto buffer = model_->buffers()->Get(tensor.buffer());
        if (!buffer)
            throw std::runtime_error("Cannot read buffer");
        return *buffer;
    }

    shape_t get_shape(const flatbuffers::Vector<int32_t> &shape)
    {
        return { std::begin(shape), std::end(shape) };
    }

    template <class T>
    xt::xarray<T> load_array(const tflite::Tensor &tensor)
    {
        auto &buffer = get_buffer<T>(tensor);
        auto shape = get_shape(*tensor.shape());
        return xt::adapt(reinterpret_cast<const T *>(buffer.data()->data()), shape);
    }

    template <class T, size_t N>
    xt::xtensor<T, N> load_tensor(const tflite::Tensor &tensor)
    {
        auto &buffer = get_buffer<T>(tensor);
        auto shape = get_shape(*tensor.shape());
        return xt::adapt(reinterpret_cast<const T *>(buffer.data()->data()), shape);
    }

    template <class T, size_t N>
    xt::xtensor<T, N> load_weights(const tflite::Tensor &tensor)
    {
        auto data = load_tensor<T, N>(tensor);
        if constexpr (N == 4)
            data = xt::transpose(data, { 0, 3, 1, 2 });
        return data;
    }

    template <class T>
    constexpr tflite::TensorType to_tensor_type()
    {
        if constexpr (std::is_same_v<T, float>)
            return tflite::TensorType_FLOAT32;
        else if constexpr (std::is_same_v<T, float>)
            return tflite::TensorType_FLOAT32;
        else if constexpr (std::is_same_v<T, int32_t>)
            return tflite::TensorType_INT32;
        else
            static_assert(false, "Invalid element type");
    }

    constexpr datatype_t to_data_type(tflite::TensorType type)
    {
        switch (type)
        {
        case tflite::TensorType_FLOAT32:
            return dt_float32;
        default:
            throw std::runtime_error("Invalid tesnor type");
        }
    }

    transpose *nhwc_to_nchw(datatype_t type, const shape_t &input_shape)
    {
        return graph_.emplace<transpose>(type, input_shape, shape_t { 0, 3, 1, 2 });
    }

    transpose *nchw_to_nhwc(datatype_t type, const shape_t &input_shape)
    {
        return graph_.emplace<transpose>(type, input_shape, shape_t { 0, 2, 3, 1 });
    }

    shape_t nhwc_to_nchw(const shape_t &input_shape)
    {
        return get_transposed_shape(input_shape, { 0, 3, 1, 2 });
    }

    shape_t nchw_to_nhwc(const shape_t &input_shape)
    {
        return get_transposed_shape(input_shape, { 0, 2, 3, 1 });
    }

    value_range<float> to_float_clamp_range(tflite::ActivationFunctionType func)
    {
        switch (func)
        {
        case tflite::ActivationFunctionType_NONE:
            return { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max() };
        case tflite::ActivationFunctionType_RELU:
            return { 0.f, std::numeric_limits<float>::max() };
        case tflite::ActivationFunctionType_RELU_N1_TO_1:
            return { -1.f, 1.f };
        case tflite::ActivationFunctionType_RELU6:
            return { 0.f, 6.f };
        default:
            throw std::runtime_error(std::string("Not supported activation: ") + tflite::EnumNameActivationFunctionType(func));
        }
    }

private:
    const tflite::Model *model_;
    const tflite::SubGraph *subGraph_;
    graph &graph_;
    std::unordered_map<input_connector *, int32_t> input_tensors_;
    std::unordered_map<int32_t, output_connector *> output_tensors_;
};

graph nncase::importer::import_tflite(xtl::span<const uint8_t> model)
{
    graph graph;
    tflite_importer(model, graph).import();
    return graph;
}
