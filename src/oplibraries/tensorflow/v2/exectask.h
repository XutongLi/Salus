/*
 * <one line to give the library's name and an idea of what it does.>
 * Copyright (C) 2017  Aetf <aetf@unlimitedcodeworks.xyz>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EXECTASK_H
#define EXECTASK_H

#include "md_executor_impl.h"

#include "execution/itask.h"

#include "oplibraries/tensorflow/tensorflow_headers.h"

namespace tensorflow {
class Device;
class FunctionLibraryRuntime;
}

struct DeviceItem
{
    tensorflow::Device *device = nullptr;
    tensorflow::FunctionLibraryRuntime *function_library = nullptr;
    bool device_record_tensor_access = false;
};

/**
 * @todo write docs
 */
namespace utils {
class semaphore;
}
class ExecTask : public ITask
{
public:
    ExecTask(ExecutorState *state, utils::semaphore *se,
             ExecutorState::TaggedNode &node, ExecutorState::TaggedNodeSeq &ready,
             ExecutorState::TaggedNodeReadyQueue &inline_ready,
             tf::NodeExecStats *stats, tf::OpKernelContext::Params &params,
             int64_t &scheduled_usec,
             ExecutorState::EntryVector &outputs,
             TensorValueVec &inputs,
             DeviceContextVec &input_device_contexts,
             AllocatorAttributeVec &input_alloc_attrs);

    bool prepare(DeviceSpec &dev) override;

    ProtoPtr run() override;

    ~ExecTask() override;

private:
    tensorflow::Status LookupDevice(const DeviceSpec &spec, DeviceItem &item);

private:
    DeviceItem ditem;

    ExecutorState::TaggedNode &tagged_node;
    ExecutorState::TaggedNodeSeq &ready;
    ExecutorState::TaggedNodeReadyQueue &inline_ready;
    tf::NodeExecStats *stats;
    tf::OpKernelContext::Params &params;
    int64_t &scheduled_usec;
    ExecutorState::EntryVector &outputs;
    TensorValueVec &inputs;
    DeviceContextVec &input_device_contexts;
    AllocatorAttributeVec &input_alloc_attrs;

    tf::OpKernel *op_kernel;
    bool kernel_is_async;
    bool completed;

    utils::semaphore *m_se;
    ExecutorState *m_state;
};

#endif // EXECTASK_H