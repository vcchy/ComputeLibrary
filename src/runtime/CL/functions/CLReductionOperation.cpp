/*
 * Copyright (c) 2017-2018 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/runtime/CL/functions/CLReductionOperation.h"

#include "arm_compute/core/CL/ICLTensor.h"
#include "arm_compute/core/CL/kernels/CLReductionOperationKernel.h"
#include "arm_compute/core/Error.h"
#include "arm_compute/core/PixelValue.h"
#include "arm_compute/core/TensorInfo.h"
#include "arm_compute/core/Validate.h"
#include "arm_compute/runtime/CL/CLScheduler.h"
#include "arm_compute/runtime/Tensor.h"
#include "support/ToolchainSupport.h"

using namespace arm_compute;

namespace
{
unsigned int calculate_number_of_stages(const ITensorInfo *input, unsigned int axis)
{
    // We need only 1 stage for all axis except x-axis and x-axis for QASYMM8.
    if(axis != 0 || (axis == 0 && is_data_type_quantized(input->data_type())))
    {
        return 1;
    }
    // Calculate number of WGs. 16 elements per thread, 8 threads per WG
    const unsigned int num_of_wg = ceil(input->dimension(0) / 128.f);

    // Calculate number of stages. First stage performs op and the rest reduction sum
    // depending on the size of the input. Last stage should have only 1 WG.
    const unsigned int num_of_stages = num_of_wg / 128 + 2;

    return num_of_stages;
}
} // namespace

CLReductionOperation::CLReductionOperation(std::shared_ptr<IMemoryManager> memory_manager)
    : _memory_group(std::move(memory_manager)), _sums_vector(), _reduction_kernels_vector(), _border_handlers_vector(), _num_of_stages(), _reduction_axis(), _is_quantized()
{
}

Status CLReductionOperation::validate(const ITensorInfo *input, const ITensorInfo *output, unsigned int axis, ReductionOperation op)
{
    const unsigned int num_of_stages = calculate_number_of_stages(input, axis);

    if(axis == 0 && !is_data_type_quantized(input->data_type()))
    {
        // Create temporary tensor infos
        auto sums_vector = arm_compute::support::cpp14::make_unique<TensorInfo[]>(num_of_stages - 1);

        // Create intermediate tensor info
        TensorShape shape{ input->tensor_shape() };

        for(unsigned int i = 0; i < num_of_stages - 1; i++)
        {
            shape.set(0, ceil(shape.x() / 128.f));
            sums_vector[i].set_data_type(input->data_type());
            sums_vector[i].set_tensor_shape(shape);
            sums_vector[i].set_num_channels(input->num_channels());
        }

        ReductionOperation first_kernel_op;
        ReductionOperation last_kernel_op;
        switch(op)
        {
            case ReductionOperation::SUM:
            case ReductionOperation::MEAN_SUM:
                first_kernel_op = ReductionOperation::SUM;
                last_kernel_op  = op;
                break;
            case ReductionOperation::SUM_SQUARE:
                first_kernel_op = ReductionOperation::SUM_SQUARE;
                last_kernel_op  = ReductionOperation::SUM;
                break;
            default:
                ARM_COMPUTE_ERROR("Not supported");
        }

        // Validate ReductionOperation only on first kernel
        ARM_COMPUTE_RETURN_ON_ERROR(CLReductionOperationKernel::validate(input, sums_vector.get(), axis, first_kernel_op));

        // Validate ReductionOperation on intermediate stages
        for(unsigned int i = 1; i < num_of_stages - 1; ++i)
        {
            ARM_COMPUTE_RETURN_ON_ERROR(CLReductionOperationKernel::validate(sums_vector.get() + i - 1, sums_vector.get() + i, axis, ReductionOperation::SUM));
        }

        // Validate ReductionOperation on the last stage
        const unsigned int last_stage = num_of_stages - 1;
        ARM_COMPUTE_RETURN_ON_ERROR(CLReductionOperationKernel::validate(sums_vector.get() + last_stage - 1, output, axis, last_kernel_op, input->dimension(0)));
    }
    else
    {
        ARM_COMPUTE_RETURN_ON_ERROR(CLReductionOperationKernel::validate(input, output, axis, op));
    }

    return Status{};
}

void CLReductionOperation::configure(ICLTensor *input, ICLTensor *output, unsigned int axis, ReductionOperation op)
{
    _num_of_stages  = calculate_number_of_stages(input->info(), axis);
    _reduction_axis = axis;
    _is_quantized   = is_data_type_quantized(input->info()->data_type());

    // Configure reduction operation kernels
    _reduction_kernels_vector = arm_compute::support::cpp14::make_unique<CLReductionOperationKernel[]>(_num_of_stages);

    // Create temporary tensors
    if(axis == 0 && !_is_quantized)
    {
        _border_handlers_vector = arm_compute::support::cpp14::make_unique<CLFillBorderKernel[]>(_num_of_stages);
        _sums_vector            = arm_compute::support::cpp14::make_unique<CLTensor[]>(_num_of_stages - 1);
        TensorShape shape{ input->info()->tensor_shape() };
        for(unsigned int i = 0; i < _num_of_stages - 1; i++)
        {
            shape.set(0, ceil(shape.x() / 128.f));
            _sums_vector[i].allocator()->init(input->info()->clone()->set_tensor_shape(shape));
        }

        // Apply ReductionOperation only on first kernel
        _memory_group.manage(_sums_vector.get());

        ReductionOperation first_kernel_op;
        ReductionOperation last_kernel_op;
        switch(op)
        {
            case ReductionOperation::SUM:
            case ReductionOperation::MEAN_SUM:
                first_kernel_op = ReductionOperation::SUM;
                last_kernel_op  = op;
                break;
            case ReductionOperation::SUM_SQUARE:
                first_kernel_op = ReductionOperation::SUM_SQUARE;
                last_kernel_op  = ReductionOperation::SUM;
                break;
            default:
                ARM_COMPUTE_ERROR("Not supported");
        }

        _reduction_kernels_vector[0].configure(input, _sums_vector.get(), axis, first_kernel_op);
        _border_handlers_vector[0].configure(input, _reduction_kernels_vector[0].border_size(), BorderMode::CONSTANT, PixelValue(0));

        // Apply ReductionOperation on intermediate stages
        for(unsigned int i = 1; i < _num_of_stages - 1; ++i)
        {
            _memory_group.manage(_sums_vector.get() + i);
            _reduction_kernels_vector[i].configure(_sums_vector.get() + i - 1, _sums_vector.get() + i, axis, ReductionOperation::SUM);
            _border_handlers_vector[i].configure(_sums_vector.get() + i - 1, _reduction_kernels_vector[i].border_size(), BorderMode::CONSTANT, PixelValue(0));
            _sums_vector[i - 1].allocator()->allocate();
        }

        // Apply ReductionOperation on the last stage
        const unsigned int last_stage  = _num_of_stages - 1;
        const unsigned int input_width = input->info()->dimension(0);
        _reduction_kernels_vector[last_stage].configure(_sums_vector.get() + last_stage - 1, output, axis, last_kernel_op, input_width);
        _border_handlers_vector[last_stage].configure(_sums_vector.get() + last_stage - 1, _reduction_kernels_vector[last_stage].border_size(), BorderMode::CONSTANT, PixelValue(0));
        _sums_vector[last_stage - 1].allocator()->allocate();
    }
    else
    {
        _reduction_kernels_vector[0].configure(input, output, axis, op, 0);
    }
}

void CLReductionOperation::run()
{
    _memory_group.acquire();

    if(_reduction_axis == 0 && !_is_quantized)
    {
        for(unsigned int i = 0; i < _num_of_stages; ++i)
        {
            CLScheduler::get().enqueue(_border_handlers_vector[i], false);
            CLScheduler::get().enqueue(_reduction_kernels_vector[i], false);
        }
    }
    else
    {
        CLScheduler::get().enqueue(_reduction_kernels_vector[0], false);
    }

    _memory_group.release();
}
