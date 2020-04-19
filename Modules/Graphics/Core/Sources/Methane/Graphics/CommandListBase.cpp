/******************************************************************************

Copyright 2019-2020 Evgeny Gorodetskiy

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*******************************************************************************

FILE: Methane/Graphics/CommandListBase.cpp
Base implementation of the command list interface.

******************************************************************************/

#include "DeviceBase.h"
#include "CommandQueueBase.h"
#include "ProgramBindingsBase.h"
#include "ResourceBase.h"

#include <Methane/Instrumentation.h>

#ifdef COMMAND_EXECUTION_LOGGING
#include <Methane/Platform/Utils.h>
#endif

#include <cassert>

// Disable debug groups instrumentation with discontinuous CPU frames in Tracy,
// because it is not working for parallel render command lists by some reason
// #define METHANE_DEBUG_GROUP_FRAMES_ENABLED
#ifndef METHANE_DEBUG_GROUP_FRAMES_ENABLED
#undef META_CPU_FRAME_START
#define META_CPU_FRAME_START(name)
#undef META_CPU_FRAME_END
#define META_CPU_FRAME_END(name)
#endif

namespace Methane::Graphics
{

std::string CommandListBase::GetStateName(State state)
{
    META_FUNCTION_TASK();
    switch (state)
    {
    case State::Pending:   return "Pending";
    case State::Committed: return "Committed";
    case State::Executing: return "Executing";
    }
    return "Undefined";
}

CommandListBase::CommandListBase(CommandQueueBase& command_queue, Type type)
    : m_type(type)
    , m_sp_command_queue(command_queue.GetPtr())
    , m_sp_command_state(CommandState::Create(type))
{
    META_FUNCTION_TASK();
}

void CommandListBase::PushDebugGroup(const std::string& name)
{
    META_FUNCTION_TASK();

#ifdef COMMAND_EXECUTION_LOGGING
    Platform::PrintToDebugOutput("Command list \"" + GetName() + "\" PUSH debug group \"" + name + "\"");
#endif

    const std::string& dict_name = PushOpenDebugGroup(name);
    META_CPU_FRAME_START(dict_name.c_str());
    META_UNUSED(dict_name);
}

void CommandListBase::PopDebugGroup()
{
    META_FUNCTION_TASK();
    if (m_open_debug_groups.empty())
    {
        throw std::underflow_error("Can not pop debug group, since no debug groups were pushed.");
    }

#ifdef COMMAND_EXECUTION_LOGGING
    Platform::PrintToDebugOutput("Command list \"" + GetName() + "\" POP debug group \"" + m_open_debug_groups.top().get() + "\"");
#endif

    META_CPU_FRAME_END(m_open_debug_groups.top().get().c_str());
    m_open_debug_groups.pop();
}

void CommandListBase::Reset(const std::string& debug_group)
{
    META_FUNCTION_TASK();
    if (m_state != State::Pending)
        throw std::logic_error("Can not reset command list in committed or executing state.");

    // NOTE: ResetCommandState() must be called from the top-most overridden Reset method

    const bool debug_group_changed = m_open_debug_groups.empty() || m_open_debug_groups.top().get() != debug_group;

    if (!m_open_debug_groups.empty() && debug_group_changed)
    {
        PopDebugGroup();
    }

    if (!debug_group.empty() && debug_group_changed)
    {
        PushDebugGroup(debug_group);
    }
}

void CommandListBase::SetProgramBindings(ProgramBindings& program_bindings, ProgramBindings::ApplyBehavior::Mask apply_behavior)
{
    META_FUNCTION_TASK();
    if (m_state != State::Pending)
        throw std::logic_error("Can not set program bindings on committed or executing command list.");

    ProgramBindingsBase& program_bindings_base = static_cast<ProgramBindingsBase&>(program_bindings);
    program_bindings_base.Apply(*this, apply_behavior);
    
    assert(!!m_sp_command_state);
    m_sp_command_state->p_program_bindings = &program_bindings_base;
}

void CommandListBase::Commit()
{
    META_FUNCTION_TASK();

    if (m_state != State::Pending)
    {
        throw std::logic_error("Command list \"" + GetName() + "\" in " + GetStateName(m_state) + " state can not be committed. Only Pending command lists can be committed.");
    }

#ifdef COMMAND_EXECUTION_LOGGING
    Platform::PrintToDebugOutput("CommandList \"" + GetName() + "\" is committed on frame " + std::to_string(GetCurrentFrameIndex()));
#endif

    m_committed_frame_index = GetCurrentFrameIndex();
    m_state = State::Committed;

    if (!m_open_debug_groups.empty())
    {
        PopDebugGroup();
    }
}

void CommandListBase::Execute(uint32_t frame_index)
{
    META_FUNCTION_TASK();

    if (m_state != State::Committed)
    {
        throw std::logic_error("Command list \"" + GetName() + "\" in " + GetStateName(m_state) + " state can not be executed. Only Committed command lists can be executed.");
    }

    if (m_committed_frame_index != frame_index)
    {
        throw std::logic_error("Command list \"" + GetName() + "\" committed on frame " + std::to_string(m_committed_frame_index) + " can not be executed on frame " + std::to_string(frame_index));
    }

#ifdef COMMAND_EXECUTION_LOGGING
    Platform::PrintToDebugOutput("CommandList \"" + GetName() + "\" is executing on frame " + std::to_string(frame_index));
#endif

    m_state = State::Executing;
}

void CommandListBase::Complete(uint32_t frame_index)
{
    META_FUNCTION_TASK();
    if (m_state != State::Executing)
    {
        throw std::logic_error("Command list \"" + GetName() + "\" in " + GetStateName(m_state) + " state can not be completed. Only Executing command lists can be completed.");
    }

    if (m_committed_frame_index != frame_index)
    {
        throw std::logic_error("Command list \"" + GetName() + "\" committed on frame " + std::to_string(m_committed_frame_index) + " can not be completed on frame " + std::to_string(frame_index));
    }

#ifdef COMMAND_EXECUTION_LOGGING
    Platform::PrintToDebugOutput("CommandList \"" + GetName() + "\" was completed on frame " + std::to_string(frame_index));
#endif

    m_state = State::Pending;
}

const std::string& CommandListBase::GetTopOpenDebugGroup() const
{
    META_FUNCTION_TASK();
    static std::string s_empty_name;
    return m_open_debug_groups.empty() ? s_empty_name : m_open_debug_groups.top().get();
}

const std::string& CommandListBase::PushOpenDebugGroup(const std::string& name)
{
    META_FUNCTION_TASK();
    // NOTE: Debug group names pooling is currently required for Tracy discontinuous frames instrumentation
    const std::string& dict_name = *m_debug_group_names.insert(name).first;
    m_open_debug_groups.emplace(dict_name);
    return dict_name;
}

void CommandListBase::ClearOpenDebugGroups()
{
    META_FUNCTION_TASK();
    while(!m_open_debug_groups.empty())
    {
        m_open_debug_groups.pop();
    }
}

bool CommandListBase::IsExecutingOnAnyFrame() const
{
    META_FUNCTION_TASK();
    return m_state == State::Executing;
}

bool CommandListBase::IsCommitted(uint32_t frame_index) const
{
    META_FUNCTION_TASK();
    return m_state == State::Committed && m_committed_frame_index == frame_index;
}

bool CommandListBase::IsExecuting(uint32_t frame_index) const
{
    META_FUNCTION_TASK();
    return m_state == State::Executing && m_committed_frame_index == frame_index;
}

CommandQueue& CommandListBase::GetCommandQueue()
{
    META_FUNCTION_TASK();
    assert(!!m_sp_command_queue);
    return static_cast<CommandQueueBase&>(*m_sp_command_queue);
}

uint32_t CommandListBase::GetCurrentFrameIndex() const
{
    META_FUNCTION_TASK();
    return  GetCommandQueueBase().GetCurrentFrameBufferIndex();
}

void CommandListBase::SetResourceTransitionBarriers(const Refs<Resource>& resources, ResourceBase::State state_before, ResourceBase::State state_after)
{
    META_FUNCTION_TASK();
    ResourceBase::Barriers resource_barriers;
    resource_barriers.reserve(resources.size());
    for (const Ref<Resource>& resource_ref : resources)
    {
        resource_barriers.push_back({
            ResourceBase::Barrier::Type::Transition,
            resource_ref.get(),
            state_before,
            state_after
        });
    }
    SetResourceBarriers(resource_barriers);
}

void CommandListBase::ResetCommandState()
{
    META_FUNCTION_TASK();
    m_sp_command_state = CommandState::Create(m_type);
}

CommandListBase::CommandState& CommandListBase::GetCommandState()
{
    META_FUNCTION_TASK();
    assert(!!m_sp_command_state);
    return *m_sp_command_state;
}

const CommandListBase::CommandState& CommandListBase::GetCommandState() const
{
    META_FUNCTION_TASK();
    assert(!!m_sp_command_state);
    return *m_sp_command_state;
}

CommandQueueBase& CommandListBase::GetCommandQueueBase()
{
    META_FUNCTION_TASK();
    return static_cast<CommandQueueBase&>(CommandListBase::GetCommandQueue());
}

const CommandQueueBase& CommandListBase::GetCommandQueueBase() const
{
    META_FUNCTION_TASK();
    assert(!!m_sp_command_queue);
    return static_cast<const CommandQueueBase&>(*m_sp_command_queue);
}

} // namespace Methane::Graphics
