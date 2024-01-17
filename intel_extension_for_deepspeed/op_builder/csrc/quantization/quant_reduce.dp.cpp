// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: Apache-2.0

// DeepSpeed Team

#include <sycl/sycl.hpp>
#include <dpct/dpct.hpp>
#include <cstdio>
#include "dequantization_utils.h"
#include "ds_kernel_utils.h"
#include "memory_access_utils.h"
#include "quantization_utils.h"
#include "reduction_utils.h"

using rop = reduce::ROpType;

/*
TODO(cmikeh2): Add implementation that better handles larger nodes. It would like make sense
to leverage some parallel reductions here to improve performance.
*/

template <int numBits, int numTensors, int totalChunks, quantize::Type quantType>
/*
DPCT1110:46: The total declared local variable size in device function dequant_reduce exceeds 128
bytes and may cause high register pressure. Consult with your hardware vendor to find the total
register size available and adjust the code, or use smaller sub-group size to avoid high register
pressure.
*/
void dequant_reduce(int8_t* reduced_data,
                    float* reduced_scales,
                    const int8_t* input_data,
                    const float* input_scales,
                    int elems_per_out_group,
                    int elems_per_in_tensor,
                    int groups_per_in_tensor,
                    int elems_per_in_group,
                    int num_tensors)
{
    sycl::group<3> tb = sycl::ext::oneapi::experimental::this_group<3>();
    sycl::sub_group warp = sycl::ext::oneapi::experimental::this_sub_group();

    // NOTE(cmikeh2): This probably could be hardcoded to a larger number,
    // but that means even stronger restrictions on the number of elements per group
    // A performance analysis here might be beneficial
    constexpr int mem_granularity = (numBits == 8) ? 8 : 4;
    constexpr int elems_per_load = mem_granularity / sizeof(int8_t);  // div by 1
    constexpr int storage_values = 16 / sizeof(sycl::half2);

    const int block_offset = tb.get_group_id()[2] * elems_per_out_group;
    const int elem_offset = tb.get_local_id()[2] * elems_per_load;
    const int base_offset = block_offset + elem_offset;
    /*
    DPCT1007:49: Migration of cooperative_groups::thread_block::group_dim is not supported.
    */
    const int stride = tb.get_group_range()[2] * elems_per_load;

    sycl::half2 local_buffer[totalChunks * storage_values];

    quantize::GroupStats<quantType> stats;

#pragma unroll
    for (int i = 0; i < totalChunks; i++) {
        sycl::half2* iteration_buffer = local_buffer + i * storage_values;

#pragma unroll
        for (int j = 0; j < storage_values; j++) {
            iteration_buffer[j] = reduce::init<rop::Add, sycl::half2>();
        }

        const int iter_offset = i * stride + base_offset;
        const int iter_scale_idx = iter_offset / elems_per_in_group;
        bool do_loads = i * stride + elem_offset < elems_per_out_group;

        if (numTensors > 0) {
#pragma unroll
            for (int j = 0; j < numTensors; j++) {
                if (do_loads) {
                    int8_t load_buffer[elems_per_load];

                    mem_access::load_global<mem_granularity>(
                        load_buffer, input_data + j * elems_per_in_tensor + iter_offset);

                    quantize::Params<quantType, numBits> params(
                        input_scales + j * groups_per_in_tensor, iter_scale_idx);

                    sycl::half2 dequant_buffer[storage_values];
                    dequantize::chunk<numBits, quantType>(dequant_buffer, load_buffer, params);

#pragma unroll
                    for (int k = 0; k < storage_values; k++) {
                        iteration_buffer[k] =
                            reduce::element<rop::Add>(iteration_buffer[k], dequant_buffer[k]);
                    }
                }
            }
        } else {
#pragma unroll 4
            for (int j = 0; j < num_tensors; j++) {
                if (do_loads) {
                    int8_t load_buffer[elems_per_load];

                    mem_access::load_global<mem_granularity>(
                        load_buffer, input_data + j * elems_per_in_tensor + iter_offset);

                    quantize::Params<quantType, numBits> params(
                        input_scales + j * groups_per_in_tensor, iter_scale_idx);

                    sycl::half2 dequant_buffer[storage_values];
                    dequantize::chunk<numBits, quantType>(dequant_buffer, load_buffer, params);

#pragma unroll
                    for (int k = 0; k < storage_values; k++) {
                        iteration_buffer[k] =
                            reduce::element<rop::Add>(iteration_buffer[k], dequant_buffer[k]);
                    }
                }
            }
        }

#pragma unroll
        for (int j = 0; j < storage_values; j++) { stats.update(iteration_buffer[j]); }
    }

    auto params = stats.template get_params<numBits, 1024>(tb, warp);

    if (tb.get_local_id()[2] == 0) { params.store(reduced_scales, tb.get_group_id()[2]); }

#pragma unroll
    for (int i = 0; i < totalChunks; i++) {
        const int iter_offset = i * stride + base_offset;
        if (i * stride + elem_offset < elems_per_out_group) {
            int8_t local_output[elems_per_load];
            quantize::_chunk<numBits, quantType>(
                local_output, local_buffer + i * storage_values, params);
            mem_access::store_global<mem_granularity>(reduced_data + iter_offset, local_output);
        }
    }
}

template <int Power>
int32_t pow2_round(int32_t raw_value)
{
    return (((raw_value - 1) >> Power) + 1) << Power;
}

/*
DPCT1049:47: The work-group size passed to the SYCL kernel may exceed the limit. To get the device
limit, query info::device::max_work_group_size. Adjust the work-group size if needed.
*/
#define LAUNCH_DEQUANT_REDUCE(num_chunks)                                                       \
 {                                                                                              \
  dpct::has_capability_or_fail(stream->get_device(), {sycl::aspect::fp64, sycl::aspect::fp16}); \
  stream->submit([&](sycl::handler& cgh) {                                                      \
   int8_t* reduced_data_ct0 = reduced_data;                                                     \
   float* reduced_scales_ct1 = reduced_scales;                                                  \
   const int8_t* input_data_ct2 = input_data;                                                   \
   const float* input_scales_ct3 = input_scales;                                                \
   auto elems_per_out_group_ct4 = elems_per_out_group;                                          \
   auto elems_per_in_tensor_ct5 = elems_per_in_tensor;                                          \
   auto groups_per_in_tensor_ct6 = groups_per_in_tensor;                                        \
   auto elems_per_in_group_ct7 = elems_per_in_group;                                            \
   auto num_tensors_ct8 = num_tensors;                                                          \
                                                                                                \
   cgh.parallel_for(sycl::nd_range<3>(grid * block, block),                                     \
                    [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {         \
                     dequant_reduce<numBits, numTensors, num_chunks, quantType>(                \
                         reduced_data_ct0,                                                      \
                         reduced_scales_ct1,                                                    \
                         input_data_ct2,                                                        \
                         input_scales_ct3,                                                      \
                         elems_per_out_group_ct4,                                               \
                         elems_per_in_tensor_ct5,                                               \
                         groups_per_in_tensor_ct6,                                              \
                         elems_per_in_group_ct7,                                                \
                         num_tensors_ct8);                                                      \
                    });                                                                         \
  });                                                                                           \
 }

template <int numBits, int numTensors, quantize::Type quantType>
void launch_dequant_reduce_impl(int8_t* reduced_data,
                                float* reduced_scales,
                                const int8_t* input_data,
                                const float* input_scales,
                                int out_groups,
                                int elems_per_out_group,
                                int elems_per_in_tensor,
                                int groups_per_in_tensor,
                                int elems_per_in_group,
                                int num_tensors,
                                dpct::queue_ptr stream)
{
    // This is a coincidence. This is derived by 8 halves per 16 bytes with 2-way packing for int4
    constexpr int elems_per_thread = numBits;
    const int one_step_threads =
        next_pow2((elems_per_out_group + elems_per_thread - 1) / (elems_per_thread));
    // TODO(cmikeh2): Tune this
    const int threads = (one_step_threads < 1024) ? one_step_threads : 1024;

    sycl::range<3> block(1, 1, threads);
    sycl::range<3> grid(1, 1, out_groups);

    const int elems_per_step = threads * elems_per_thread;
    const int unroll_raw = (elems_per_out_group + elems_per_step - 1) / elems_per_step;

    const int unroll = (unroll_raw >= 4) ? pow2_round<1>(unroll_raw) : unroll_raw;

    if (unroll == 1) {
        // 0-4096 elems
        LAUNCH_DEQUANT_REDUCE(1);
    } else if (unroll == 2) {
        // 4097-8192 etc...
        LAUNCH_DEQUANT_REDUCE(2);
    } else if (unroll == 3) {
        LAUNCH_DEQUANT_REDUCE(3);
    } else if (unroll == 4) {
        LAUNCH_DEQUANT_REDUCE(4);
    } else if (unroll == 6) {
        LAUNCH_DEQUANT_REDUCE(6);
    } else if (unroll == 8) {
        LAUNCH_DEQUANT_REDUCE(8);
    } else if (unroll == 10) {
        LAUNCH_DEQUANT_REDUCE(10);
    } else if (unroll == 12) {
        // 48k limit
        LAUNCH_DEQUANT_REDUCE(12);
    } else {
        assert(false);
    }
}

#define LAUNCH_DEQUANT_REDUCE_IMPL(NUM_BITS, NUM_GPUS, QUANT_TYPE)                   \
    launch_dequant_reduce_impl<NUM_BITS, NUM_GPUS, QUANT_TYPE>(reduced_data,         \
                                                               reduced_scales,       \
                                                               input_data,           \
                                                               input_scales,         \
                                                               out_groups,           \
                                                               elems_per_out_group,  \
                                                               elems_per_in_tensor,  \
                                                               groups_per_in_tensor, \
                                                               elems_per_in_group,   \
                                                               num_gpus,             \
                                                               stream);

void launch_dequant_reduce(int8_t* reduced_data,
                           float* reduced_scales,
                           const int8_t* input_data,
                           const float* input_scales,
                           int num_gpus,
                           int num_bits,
                           quantize::Type quant_type,
                           int out_groups,
                           int elems_per_out_group,
                           int elems_per_in_tensor,
                           int groups_per_in_tensor,
                           int elems_per_in_group,
                           dpct::queue_ptr stream)
{
    if (quant_type == quantize::Type::Symmetric) {
        if (num_bits == 4) {
            if (num_gpus == 8) {
                LAUNCH_DEQUANT_REDUCE_IMPL(4, 8, quantize::Type::Symmetric);
            } else if (num_gpus == 16) {
                LAUNCH_DEQUANT_REDUCE_IMPL(4, 16, quantize::Type::Symmetric);
            } else {
                LAUNCH_DEQUANT_REDUCE_IMPL(4, -1, quantize::Type::Symmetric);
            }
        } else if (num_bits == 8) {
            if (num_gpus == 8) {
                LAUNCH_DEQUANT_REDUCE_IMPL(8, 8, quantize::Type::Symmetric);
            } else if (num_gpus == 16) {
                LAUNCH_DEQUANT_REDUCE_IMPL(8, 16, quantize::Type::Symmetric);
            } else {
                LAUNCH_DEQUANT_REDUCE_IMPL(8, -1, quantize::Type::Symmetric);
            }
        }
    } else if (quant_type == quantize::Type::Asymmetric) {
        if (num_bits == 4) {
            if (num_gpus == 8) {
                LAUNCH_DEQUANT_REDUCE_IMPL(4, 8, quantize::Type::Asymmetric);
            } else if (num_gpus == 16) {
                LAUNCH_DEQUANT_REDUCE_IMPL(4, 16, quantize::Type::Asymmetric);
            } else {
                LAUNCH_DEQUANT_REDUCE_IMPL(4, -1, quantize::Type::Asymmetric);
            }
        } else if (num_bits == 8) {
            if (num_gpus == 8) {
                LAUNCH_DEQUANT_REDUCE_IMPL(8, 8, quantize::Type::Asymmetric);
            } else if (num_gpus == 16) {
                LAUNCH_DEQUANT_REDUCE_IMPL(8, 16, quantize::Type::Asymmetric);
            } else {
                LAUNCH_DEQUANT_REDUCE_IMPL(8, -1, quantize::Type::Asymmetric);
            }
        }
    }
}
