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

FILE: Methane/Graphics/ResourceManager.cpp
Resource manager used as a central place for creating and accessing descriptor heaps
and deferred releasing of GPU resource.

******************************************************************************/

#include "ResourceManager.h"

#include <Methane/Instrumentation.h>
#include <Methane/Data/Parallel.hpp>

#include <cassert>

namespace Methane::Graphics
{

ResourceManager::ResourceManager(ContextBase& context)
    : m_context(context)
    , m_sp_release_pool(ResourceBase::ReleasePool::Create())
{
    META_FUNCTION_TASK();
}

void ResourceManager::Initialize(const Settings& settings)
{
    META_FUNCTION_TASK();

    m_deferred_heap_allocation = settings.deferred_heap_allocation;
    for (uint32_t heap_type_idx = 0; heap_type_idx < static_cast<uint32_t>(DescriptorHeap::Type::Count); ++heap_type_idx)
    {
        const DescriptorHeap::Type heap_type  = static_cast<DescriptorHeap::Type>(heap_type_idx);
        Ptrs<DescriptorHeap>&      desc_heaps = m_descriptor_heap_types[heap_type_idx];
        desc_heaps.clear();

        // CPU only accessible descriptor heaps of all types are created for default resource creation
        {
            const uint32_t                 heap_size     = settings.default_heap_sizes[heap_type_idx];
            const DescriptorHeap::Settings heap_settings{ heap_type, heap_size, m_deferred_heap_allocation, false };
            desc_heaps.push_back(DescriptorHeap::Create(m_context, heap_settings));
        }

        // GPU accessible descriptor heaps are created for program resource bindings
        if (DescriptorHeap::IsShaderVisibleHeapType(heap_type))
        {
            const uint32_t                 heap_size    = settings.shader_visible_heap_sizes[heap_type_idx];
            const DescriptorHeap::Settings heap_settings{ heap_type, heap_size, m_deferred_heap_allocation, true };
            desc_heaps.push_back(DescriptorHeap::Create(m_context, heap_settings));
        }
    }
}

void ResourceManager::CompleteInitialization()
{
    META_FUNCTION_TASK();
    if (!IsDeferredHeapAllocation())
        return;

    std::lock_guard<LockableBase(std::mutex)> lock_guard(m_program_bindings_mutex);

    for (const Ptrs<DescriptorHeap>& desc_heaps : m_descriptor_heap_types)
    {
        for (const Ptr<DescriptorHeap>& sp_desc_heap : desc_heaps)
        {
            assert(!!sp_desc_heap);
            sp_desc_heap->Allocate();
        }
    }

    const auto program_bindings_end_it = std::remove_if(m_program_bindings.begin(), m_program_bindings.end(),
        [](const WeakPtr<ProgramBindings>& wp_program_bindings)
        { return wp_program_bindings.expired(); }
    );

    m_program_bindings.erase(program_bindings_end_it, m_program_bindings.end());

    Data::ParallelForEach<WeakPtrs<ProgramBindings>::const_iterator, const WeakPtr<ProgramBindings>>(
        m_program_bindings.begin(), m_program_bindings.end(),
        [](const WeakPtr<ProgramBindings>& wp_program_bindings)
        {
            META_FUNCTION_TASK();
            Ptr<ProgramBindings> sp_program_bindings = wp_program_bindings.lock();
            assert(!!sp_program_bindings);
            static_cast<ProgramBindingsBase&>(*sp_program_bindings).CompleteInitialization();
        }
    );
}

void ResourceManager::Release()
{
    META_FUNCTION_TASK();

    if (m_sp_release_pool)
    {
        m_sp_release_pool->ReleaseResources();
    }

    for (Ptrs<DescriptorHeap>& desc_heaps : m_descriptor_heap_types)
    {
        desc_heaps.clear();
    }
}

void ResourceManager::SetDeferredHeapAllocation(bool deferred_heap_allocation)
{
    META_FUNCTION_TASK();
    if (m_deferred_heap_allocation == deferred_heap_allocation)
        return;

    m_deferred_heap_allocation = deferred_heap_allocation;
    ForEachDescriptorHeap([deferred_heap_allocation](DescriptorHeap& descriptor_heap)
    {
        descriptor_heap.SetDeferredAllocation(deferred_heap_allocation);
    });
}

void ResourceManager::AddProgramBindings(ProgramBindings& program_bindings)
{
    META_FUNCTION_TASK();

    std::lock_guard<LockableBase(std::mutex)> lock_guard(m_program_bindings_mutex);
#ifdef _DEBUG
    // This may cause performance drop on adding massive amount of program bindings,
    // so we assume that only different program bindings are added and check it in Debug builds only
    const auto program_bindings_it = std::find_if(m_program_bindings.begin(), m_program_bindings.end(),
        [&program_bindings](const WeakPtr<ProgramBindings>& sp_program_bindings)
        { return !sp_program_bindings.expired() && sp_program_bindings.lock().get() == std::addressof(program_bindings); }
    );
    assert(program_bindings_it == m_program_bindings.end());
    if (program_bindings_it != m_program_bindings.end())
        return;
#endif
    m_program_bindings.push_back(static_cast<ProgramBindingsBase&>(program_bindings).GetPtr());
}

uint32_t ResourceManager::CreateDescriptorHeap(const DescriptorHeap::Settings& settings)
{
    META_FUNCTION_TASK();

    if (settings.type == DescriptorHeap::Type::Undefined ||
        settings.type == DescriptorHeap::Type::Count)
    {
        throw std::invalid_argument("Can not create \"Undefined\" descriptor heap.");
    }
    Ptrs<DescriptorHeap>& desc_heaps = m_descriptor_heap_types[static_cast<size_t>(settings.type)];
    desc_heaps.push_back(DescriptorHeap::Create(m_context, settings));
    return static_cast<uint32_t>(desc_heaps.size() - 1);
}

const Ptr<DescriptorHeap>& ResourceManager::GetDescriptorHeapPtr(DescriptorHeap::Type type, Data::Index heap_index)
{
    META_FUNCTION_TASK();

    if (type == DescriptorHeap::Type::Undefined ||
        type == DescriptorHeap::Type::Count)
    {
        static const Ptr<DescriptorHeap> s_empty_ptr;
        return s_empty_ptr;
    }

    Ptrs<DescriptorHeap>& desc_heaps = m_descriptor_heap_types[static_cast<size_t>(type)];
    if (heap_index >= desc_heaps.size())
    {
        throw std::invalid_argument("There is no \"" + DescriptorHeap::GetTypeName(type) +
                                    "\" descriptor heap at index " + std::to_string(heap_index) +
                                    " (there are only " + std::to_string(desc_heaps.size()) + " heaps of this type).");
    }
    return desc_heaps[heap_index];
}

DescriptorHeap& ResourceManager::GetDescriptorHeap(DescriptorHeap::Type type, Data::Index heap_index)
{
    META_FUNCTION_TASK();

    if (type == DescriptorHeap::Type::Undefined ||
        type == DescriptorHeap::Type::Count)
    {
        throw std::invalid_argument("Can not get reference to \"Undefined\" descriptor heap.");
    }
    const Ptr<DescriptorHeap>& sp_resource_heap = GetDescriptorHeapPtr(type, heap_index);
    if (!sp_resource_heap)
    {
        throw std::invalid_argument("Descriptor heap of type \"" + DescriptorHeap::GetTypeName(type) +
                                    "\" and index " + std::to_string(heap_index) + " does not exist.");
    }
    return *sp_resource_heap;
}

const Ptr<DescriptorHeap>&  ResourceManager::GetDefaultShaderVisibleDescriptorHeapPtr(DescriptorHeap::Type type) const
{
    META_FUNCTION_TASK();

    if (type == DescriptorHeap::Type::Undefined ||
        type == DescriptorHeap::Type::Count)
    {
        static const Ptr<DescriptorHeap> s_empty_ptr;
        return s_empty_ptr;
    }

    const Ptrs<DescriptorHeap>& descriptor_heaps = m_descriptor_heap_types[static_cast<uint32_t>(type)];
    auto descriptor_heaps_it = std::find_if(descriptor_heaps.begin(), descriptor_heaps.end(),
        [](const Ptr<DescriptorHeap>& sp_descriptor_heap)
        {
            assert(sp_descriptor_heap);
            return sp_descriptor_heap && sp_descriptor_heap->GetSettings().shader_visible;
        });

    static const Ptr<DescriptorHeap> s_empty_heap_ptr;
    return descriptor_heaps_it != descriptor_heaps.end() ? *descriptor_heaps_it : s_empty_heap_ptr;
}

DescriptorHeap& ResourceManager::GetDefaultShaderVisibleDescriptorHeap(DescriptorHeap::Type type) const
{
    META_FUNCTION_TASK();

    const Ptr<DescriptorHeap>& sp_resource_heap = GetDefaultShaderVisibleDescriptorHeapPtr(type);
    if (!sp_resource_heap)
    {
        throw std::invalid_argument("There is no shader visible descriptor heap of type \"" + DescriptorHeap::GetTypeName(type) + "\".");
    }
    return *sp_resource_heap;
}

ResourceManager::DescriptorHeapSizeByType ResourceManager::GetDescriptorHeapSizes(bool get_allocated_size, bool for_shader_visible_heaps) const
{
    META_FUNCTION_TASK();

    DescriptorHeapSizeByType max_descriptor_heap_sizes = {};
    ForEachDescriptorHeap([&](DescriptorHeap& descriptor_heap)
    {
        if ( (for_shader_visible_heaps && !descriptor_heap.IsShaderVisible()) ||
            (!for_shader_visible_heaps &&  descriptor_heap.IsShaderVisible()) )
            return;

        const uint32_t heap_size = get_allocated_size ? descriptor_heap.GetAllocatedSize() : descriptor_heap.GetDeferredSize();
        uint32_t& max_desc_heap_size = max_descriptor_heap_sizes[static_cast<size_t>(descriptor_heap.GetSettings().type)];
        max_desc_heap_size = std::max(max_desc_heap_size, heap_size);
    });

    return max_descriptor_heap_sizes;
}

void ResourceManager::ForEachDescriptorHeap(const std::function<void(DescriptorHeap& descriptor_heap)>& process_heap) const
{
    META_FUNCTION_TASK();
    for (uint32_t heap_type_idx = 0; heap_type_idx < static_cast<uint32_t>(DescriptorHeap::Type::Count); ++heap_type_idx)
    {
        const DescriptorHeap::Type  desc_heaps_type = static_cast<DescriptorHeap::Type>(heap_type_idx);
        const Ptrs<DescriptorHeap>& desc_heaps = m_descriptor_heap_types[heap_type_idx];
        for (const Ptr<DescriptorHeap>& sp_desc_heap : desc_heaps)
        {
            if (!sp_desc_heap)
                throw std::logic_error("Empty descriptor heap pointer should not be stored in resource manager.");

            const DescriptorHeap::Type heap_type = sp_desc_heap->GetSettings().type;
            if (heap_type != desc_heaps_type)
                throw std::logic_error("Wrong type of descriptor heap (" + DescriptorHeap::GetTypeName(heap_type) +
                                       ") was found in container assuming heaps of " + DescriptorHeap::GetTypeName(desc_heaps_type));

            process_heap(*sp_desc_heap);
        }
    }
}

ResourceBase::ReleasePool& ResourceManager::GetReleasePool()
{
    META_FUNCTION_TASK();
    assert(!!m_sp_release_pool);
    return *m_sp_release_pool;
}

} // namespace Methane::Graphics
