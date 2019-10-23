/* *** ** *** ** *** ** *** *
 * Part of rnjin            *
 * (c) Rajin Shankar, 2019  *
 *        rajinshankar.com  *
 * *** ** *** ** *** ** *** */

#include "vulkan_resources.hpp"

namespace rnjin::graphics::vulkan
{
/* -------------------------------------------------------------------------- */
/*                              Buffer Allocation                             */
/* -------------------------------------------------------------------------- */
#pragma region buffer_allocation

    buffer_allocation::buffer_allocation() : offset( 0 ), size( 0 ), buffer( nullptr ) {}
    buffer_allocation::buffer_allocation( vk::DeviceSize offset, vk::DeviceSize size, vk::Buffer* buffer )
      : pass_member( offset ), //
        pass_member( size ),   //
        pass_member( buffer )  //
    {}

#pragma endregion buffer_allocation


/* -------------------------------------------------------------------------- */
/*                              Buffer Allocator                              */
/* -------------------------------------------------------------------------- */
#pragma region buffer_allocator

    uint find_best_memory_type( vk::PhysicalDevice device, bitmask type_filter, vk::MemoryPropertyFlags target_properties );

    buffer_allocator::buffer_allocator( const device& device_instance, vk::BufferUsageFlags usage_flags, vk::MemoryPropertyFlags memory_property_flags )
      : pass_member( device_instance ),       //
        pass_member( usage_flags ),           //
        pass_member( memory_property_flags ), //
        size( 0 ),                            //
        entry_block( 0, 0, nullptr, nullptr ) //
    {}

    buffer_allocator::~buffer_allocator()
    {
        vulkan_log_verbose.print( "Destroying Vulkan buffer allocator" );
        vulkan_log_verbose.print_additional( "\1 bytes total, \2 bytes free", size, available_space );

        // Make sure no operations are in-progress before freeing buffers and memory
        device_instance.wait_for_idle();

        let& vulkan_device = device_instance.get_vulkan_device();

        // Make sure the buffer was actually created
        if ( buffer )
        {
            vulkan_device.destroyBuffer( buffer );
        }

        // Make sure the memory was actually allocated
        if ( memory )
        {
            vulkan_device.freeMemory( memory );
        }

        // Free memory used by linked list of free blocks
        for ( block* current_block = entry_block.next; current_block != nullptr; )
        {
            block* next_block = current_block->next;
            delete current_block;
            current_block = next_block;
        }
    }

    void buffer_allocator::initialize( vk::DeviceSize total_size )
    {
        let task = vulkan_log_verbose.track_scope( "Vulkan buffer allocator initialization" );

        size            = total_size;
        available_space = total_size;

        let& vulkan_device = device_instance.get_vulkan_device();

        let buffer_sharing_mode = vk::SharingMode::eExclusive;
        vk::BufferCreateInfo buffer_info(
            {},                 // flags
            size,               // size
            usage_flags,        // usage
            buffer_sharing_mode // sharingMode
        );

        // Create the buffer (no memory allocated yet)
        buffer = device_instance.get_vulkan_device().createBuffer( buffer_info );

        // Get memory information based on buffer requirements
        let buffer_memory_requirements = vulkan_device.getBufferMemoryRequirements( buffer );
        let buffer_memory_type_index   = find_best_memory_type( device_instance.get_physical_device(), buffer_memory_requirements.memoryTypeBits, memory_property_flags );

        vulkan_log_verbose.print_additional( "Allocating \1 bytes for buffer space", total_size );

        // Allocate memory for the buffer
        vk::MemoryAllocateInfo buffer_allocation_info(
            buffer_memory_requirements.size, // allocationSize
            buffer_memory_type_index         // memoryTypeIndex
        );
        memory = vulkan_device.allocateMemory( buffer_allocation_info );
        check_error_condition( return, vulkan_log_errors, not memory, "Failed to allocate device memory for Vulkan buffer object (size=\1)", size );

        vulkan_device.bindBufferMemory(
            buffer, // buffer
            memory, // memory
            0       // memoryOffset
        );

        // Initialize the first block of free memory
        block* first_block = new block( total_size, 0, &entry_block, nullptr );

        subregion
        {
            // The entry block will never be allocated to, since its size is set to 0
            entry_block.offset   = 0;
            entry_block.size     = 0;
            entry_block.previous = nullptr;
            entry_block.next     = first_block;
        }
    }

    // Reserve a portion of the whole buffer to use for an allocation of the given size
    // note: uses a first-fit allocation strategy
    buffer_allocation buffer_allocator::allocate( vk::DeviceSize size )
    {
        vulkan_log_verbose.print( "Allocating \1 byte Vulkan buffer (\2 bytes free)", size, available_space );

        check_error_condition( return buffer_allocation(), vulkan_log_errors, size == 0, "Tried to allocate 0 bytes of GPU memory" );

        block* destination_block = nullptr;
        for ( block* current_block = &entry_block; current_block != nullptr; current_block = current_block->next )
        {
            if ( current_block->size >= size )
            {
                // Break once the first block that can fit the request is found
                destination_block = current_block;
                break;
            }
        }

        check_error_condition( return buffer_allocation(), vulkan_log_errors, destination_block == nullptr, "Failed to allocate GPU memory for a request of size \1", size );

        available_space -= size;

        // The destination block size matches the request exactly, so just get rid of it
        if ( destination_block->size == size )
        {
            // note: previous is guaranteed to exist, as the entry block (the only block without a previous entry)
            //       has 0 size and will thus never be selected for allocation
            destination_block->previous->next = destination_block->next;
            let result                        = buffer_allocation( destination_block->offset, size, &buffer );
            delete destination_block;
            return result;
        }
        else
        {
            // The destination block size is larger than the request, so reserve the first part for the allocation and adjust values
            let result = buffer_allocation( destination_block->offset, size, &buffer );
            destination_block->size -= size;
            destination_block->offset += size;
            return result;
        }
    }

    // Release a portion of the whole buffer that was previously allocated, coalescing the list of free blocks as needed
    // note: should only be called for buffer_allocation objects that this instance allocated; this isn't enforced
    //       (maybe we should store an 'owner' pointer in the allocation and check that here?)
    void buffer_allocator::free( buffer_allocation& allocation )
    {
        vulkan_log_verbose.print( "Freeing \1 byte Vulkan buffer", allocation.size );

        block* previous_block = &entry_block;
        block* next_block     = nullptr;

        for ( block* current_block = &entry_block; current_block != nullptr; current_block = current_block->next )
        {
            if ( current_block->offset < allocation.offset )
            {
                previous_block = current_block;
            }
            else if ( current_block->offset > allocation.offset )
            {
                // Break once the first block beyond the allocation's offset is found
                // note: technically, the next block could only start at allocation.offset + allocation.size,
                //       but no other blocks could be allocated in this space, so this check is sufficient
                next_block = current_block;
                break;
            }
        }

        available_space += allocation.size;

        // Insert new free block and coalesce as needed

        if ( next_block == nullptr )
        {
            // No next block was found, so the allocation will be returned to the end of the list

            if ( previous_block->offset + previous_block->size == allocation.offset )
            {
                // The start of this block is the same as the end of the previous block, so just add to its size
                previous_block->size += allocation.size;
            }
            else
            {
                // Create a new block and link it onto the end of the list
                block* new_block     = new block( allocation.offset, allocation.size, previous_block, nullptr );
                previous_block->next = new_block;
            }
        }
        else if ( previous_block == &entry_block )
        {
            // This block is being returned to the front of the list
            // note: we don't want to coalesce with the entry block

            if ( allocation.offset + allocation.size == next_block->offset )
            {
                // The allocation is adjacent to the next free block, so just update its size of offset
                next_block->offset -= allocation.size;
                next_block->size += allocation.size;
            }
            else
            {
                // There is space between the allocation and the next free block, so create a new block
                // and link it into the list as appropriate

                block* new_block     = new block( allocation.size, allocation.offset, previous_block, next_block );
                previous_block->next = new_block;
                next_block->previous = new_block;
            }
        }
        else
        {
            // There is a previous (non-entry) and next block

            if ( previous_block->offset + previous_block->size + allocation.size == next_block->offset )
            {
                // This block perfectly fills the space between the previous and next blocks, so go ahead and merge the two
                // note: updates previous_block and deletes next_block to perform merge

                previous_block->size += allocation.size + next_block->size;
                previous_block->next = next_block->next;

                if ( next_block->next != nullptr )
                {
                    // Update the previous pointer of the next (noncontiguous) block to point to the newly coalesced block
                    next_block->next->previous = previous_block;
                }

                // Get rid of the 'third' coalesced block
                delete next_block;
            }
            else if ( previous_block->offset + previous_block->size == allocation.offset )
            {
                // This block is adjacent to the previous block, so just increase its size
                previous_block->size += allocation.size;
            }
            else if ( allocation.offset + allocation.size == next_block->offset )
            {
                // This block is adjacent to the next block, so update its offset and size
                next_block->offset -= allocation.size;
                next_block->size += allocation.size;
            }
            else
            {
                // This block is not adjacent to either neighboring blocks, so create a new block and link it into the list
                block* new_block     = new block( allocation.offset, allocation.size, previous_block, next_block );
                previous_block->next = new_block;
                next_block->previous = new_block;
            }
        }

        // Zero out allocation data so it isn't accidentally used in the future
        allocation.offset = 0;
        allocation.size   = 0;
        allocation.buffer = nullptr;
    }

    // Static helper function to find a suitable memory type (based on type_filter) that has all the required property flags (target_properties)
    uint find_best_memory_type( vk::PhysicalDevice device, bitmask type_filter, vk::MemoryPropertyFlags target_properties )
    {
        let device_memory_properties = device.getMemoryProperties();

        // Go through all available memory types
        for ( uint i : range( device_memory_properties.memoryTypeCount ) )
        {
            let is_suitable_type = type_filter[i];
            if ( is_suitable_type )
            {
                // Check that the current memory type has all the target flags
                let device_memory_property_flags = device_memory_properties.memoryTypes[i].propertyFlags;
                let has_target_properties        = ( device_memory_property_flags & target_properties ) == target_properties;

                if ( has_target_properties )
                {
                    return i;
                }
            }
        }

        // No memory type was found that is suitable and had all the needed flags
        const bool failed_to_find_memory_type = true;
        check_error_condition( return 0, vulkan_log_errors, failed_to_find_memory_type == true, "Failed to find a suitable memory type" );
    }

#pragma endregion buffer_allocator

/* -------------------------------------------------------------------------- */
/*                               Render Pipeline                              */
/* -------------------------------------------------------------------------- */
#pragma region render_pipeline

    render_pipeline::render_pipeline() {}
    render_pipeline::render_pipeline( vk::Pipeline vulkan_pipeline, vk::PipelineLayout layout )
      : pass_member( vulkan_pipeline ), //
        pass_member( layout )           //
    {}
    render_pipeline::~render_pipeline() {}

    void render_pipeline::invalidate()
    {
        vulkan_pipeline = vk::Pipeline();
        layout          = vk::PipelineLayout();
    }

#pragma endregion render_pipeline

/* -------------------------------------------------------------------------- */
/*                              Resource Database                             */
/* -------------------------------------------------------------------------- */
#pragma region resource_database

    resource_database::resource_database( const device& device_instance )
      : pass_member( device_instance ), //
        vertex_buffer_allocator(
            device_instance,                                                                //
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer, //
            vk::MemoryPropertyFlagBits::eDeviceLocal ),                                     //
        index_buffer_allocator(
            device_instance,                                                               //
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer, //
            vk::MemoryPropertyFlagBits::eDeviceLocal ),                                    //
        staging_buffer_allocator(
            device_instance,                                                                       //
            vk::BufferUsageFlagBits::eTransferSrc,                                                 //
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent ) //
    {}
    resource_database::~resource_database()
    {
        clean_up();
    }

    void resource_database::initialize( usize vertex_buffer_space, usize index_buffer_space, usize staging_buffer_space )
    {
        vulkan_log_verbose.print( "Initializing Vulkan resource database" );
        vulkan_log_verbose.print_additional( "\1 bytes for vertex buffers", vertex_buffer_space );
        vulkan_log_verbose.print_additional( "\1 bytes for index buffers", index_buffer_space );
        vulkan_log_verbose.print_additional( "\1 bytes for staging buffers", staging_buffer_space );

        vertex_buffer_allocator.initialize( vertex_buffer_space );
        index_buffer_allocator.initialize( index_buffer_space );
        staging_buffer_allocator.initialize( staging_buffer_space );

        let pipeline_cache_info = vk::PipelineCacheCreateInfo(
            // TODO: load pipeline cache from disk and such
        );
        pipeline_cache = device_instance.get_vulkan_device().createPipelineCache( pipeline_cache_info );
    }

    void resource_database::clean_up()
    {
        let task = vulkan_log_verbose.track_scope( "Vulkan resource database cleanup" );

        // Make sure no rendering operations are in progress when we release resources
        device_instance.wait_for_idle();

        let& vulkan_device = device_instance.get_vulkan_device();

        vulkan_device.destroyPipelineCache( pipeline_cache );
        foreach ( pipeline : pipelines )
        {
            vulkan_device.destroyPipeline( pipeline.get_vulkan_pipeline() );
            vulkan_device.destroyPipelineLayout( pipeline.get_layout() );
        }
    }

    // Copy host memory to CPU-accessible device memory owned by a staging buffer
    void resource_database::write_staging_buffer( const buffer_allocation& staging_buffer_allocation, vk::DeviceSize size, void* source )
    {
        let& vulkan_device = device_instance.get_vulkan_device();

        void* device_memory;
        let memory_map_flags = vk::MemoryMapFlags();

        vulkan_device.mapMemory(
            staging_buffer_allocator.get_memory(),  // memory
            staging_buffer_allocation.get_offset(), // offset
            staging_buffer_allocation.get_size(),   // size
            memory_map_flags,                       // flags
            &device_memory                          // ppData
        );
        memcpy( device_memory, source, size );
        vulkan_device.unmapMemory( staging_buffer_allocator.get_memory() );
    }

    // Copy CPU-accessible device memory to higher performance internal device memory
    // note: must record and submit a command buffer, so this method waits for that operation to complete before returning
    void resource_database::transfer_staging_buffer( const buffer_allocation& staging_buffer_allocation, const buffer_allocation& target_allocation )
    {
        let& vulkan_device = device_instance.get_vulkan_device();

        // Allocate a command buffer
        vk::CommandBufferAllocateInfo command_buffer_allocation_info(
            device_instance.command_pool.transfer(), // commandPool
            vk::CommandBufferLevel::ePrimary,        // level
            1                                        // commandBufferCount
        );
        vk::CommandBuffer transfer_command_buffer = vulkan_device.allocateCommandBuffers( command_buffer_allocation_info )[0];
        check_error_condition( return, vulkan_log_errors, not transfer_command_buffer, "Failed to create transfer command buffer" );

        // Prep transfer command buffer info structs
        vk::CommandBufferBeginInfo transfer_begin_info(    //
            vk::CommandBufferUsageFlagBits::eOneTimeSubmit // flags
        );
        vk::BufferCopy buffer_copy_info(
            staging_buffer_allocation.get_offset(), // srcOffset
            target_allocation.get_offset(),         // dstOffset
            staging_buffer_allocation.get_size()    // size
        );
        vk::SubmitInfo transfer_submit_info(
            0,                        // waitSemaphoreCount
            nullptr,                  // pWaitSemaphores
            {},                       // pWaitDstStageMask
            1,                        // commandBufferCount
            &transfer_command_buffer, // pCommandBuffers
            0,                        // signalSemaphoreCount
            nullptr                   // pSignalSemaphores
        );
        let no_fence = vk::Fence();

        // Record the transfer command buffer and immediately submit
        transfer_command_buffer.begin( transfer_begin_info );
        transfer_command_buffer.copyBuffer( staging_buffer_allocation.get_buffer(), target_allocation.get_buffer(), 1, &buffer_copy_info );
        transfer_command_buffer.end();

        device_instance.queue.graphics().submit( 1, &transfer_submit_info, no_fence );

        // Wait for the transfer to complete before continuing
        device_instance.queue.graphics().waitIdle();

        // Free transfer command buffer
        vulkan_device.freeCommandBuffers( device_instance.command_pool.transfer(), 1, &transfer_command_buffer );
    }

    // Release resources used by a staging buffer
    void resource_database::free_staging_buffer( buffer_allocation& staging_buffer_allocation )
    {
        staging_buffer_allocator.free( staging_buffer_allocation );
    }

    // Allocate a vertex buffer in internal device memory and transfer mesh data using a staging buffer
    buffer_allocation resource_database::create_vertex_buffer( const list<mesh::vertex>& vertices )
    {
        let buffer_size = sizeof( mesh::vertex ) * vertices.size();

        buffer_allocation new_vertex_buffer = vertex_buffer_allocator.allocate( buffer_size );

        // TODO: aggregate transfer requests and execute all at once, rather than creating, writing, and destroying staging buffers individually
        buffer_allocation new_staging_buffer = staging_buffer_allocator.allocate( buffer_size );
        write_staging_buffer( new_staging_buffer, buffer_size, (void*) vertices.data() );
        transfer_staging_buffer( new_staging_buffer, new_vertex_buffer );
        free_staging_buffer( new_staging_buffer );

        return new_vertex_buffer;
    }

    // Allocate an index buffer in internal device memory and transfer mesh data using a staging buffer
    buffer_allocation resource_database::create_index_buffer( const list<mesh::index>& indices )
    {
        let buffer_size = sizeof( mesh::index ) * indices.size();

        buffer_allocation new_index_buffer = index_buffer_allocator.allocate( buffer_size );

        // TODO: aggregate transfer requests and execute all at once, rather than creating, writing, and destroying staging buffers individually
        buffer_allocation new_staging_buffer = staging_buffer_allocator.allocate( buffer_size );
        write_staging_buffer( new_staging_buffer, buffer_size, (void*) indices.data() );
        transfer_staging_buffer( new_staging_buffer, new_index_buffer );
        free_staging_buffer( new_staging_buffer );

        return new_index_buffer;
    }

    // Free resources used by a mesh vertex buffer
    void resource_database::free_vertex_buffer( buffer_allocation& allocation )
    {
        vertex_buffer_allocator.free( allocation );
    }

    // Free resources used by a mesh index buffer
    void resource_database::free_index_buffer( buffer_allocation& allocation )
    {
        index_buffer_allocator.free( allocation );
    }

    // Helper structures/functions for pipeline creation
    struct vertex_description
    {
        vk::VertexInputBindingDescription input;
        vk::VertexInputAttributeDescription* input_attributes;
        uint input_attribute_count;
    };
    template <typename T>
    static vertex_description get_vertex_binding_description();
    template <>
    static vertex_description get_vertex_binding_description<mesh::vertex>();

    render_pipeline resource_database::create_pipeline( const shader& vertex_shader, const shader& fragment_shader, const vk::RenderPass& render_pass )
    {
        let& vulkan_device = device_instance.get_vulkan_device();

        vk::Pipeline new_pipeline;
        vk::PipelineLayout new_pipeline_layout;

        tracked_subregion( vulkan_log_verbose, "Vulkan pipeline creation" )
        {
            // Pipeline layout info
            // TODO: handle descriptor sets and push constants
            vk::PipelineLayoutCreateInfo pipeline_layout_info(
                {},      // flags
                0,       // setLayoutCount
                nullptr, // pSetLayouts
                0,       // pushConstantRangeCount
                nullptr  // pPushConstantRanges
            );

            new_pipeline_layout = vulkan_device.createPipelineLayout( pipeline_layout_info );
            check_error_condition( return render_pipeline(), vulkan_log_errors, not new_pipeline_layout, "Failed to create Vulkan pipeline layout" );

            // Get shader bytecode for each stage
            let& vertex_shader_binary   = vertex_shader.get_spirv();
            let& fragment_shader_binary = fragment_shader.get_spirv();

            // Build shader modules
            vk::ShaderModuleCreateInfo vertex_shader_info(
                {},                                                         // flags
                vertex_shader_binary.size() * sizeof( shader::spirv_char ), // codeSize
                vertex_shader_binary.data()                                 // pCode
            );
            vk::ShaderModuleCreateInfo fragment_shader_info(
                {},                                                           // flags
                fragment_shader_binary.size() * sizeof( shader::spirv_char ), // codeSize
                fragment_shader_binary.data()                                 // pCode
            );

            // note: these will be cleaned up automatically since they're made with a createUnique call
            let vertex_shader_module   = vulkan_device.createShaderModuleUnique( vertex_shader_info );
            let fragment_shader_module = vulkan_device.createShaderModuleUnique( fragment_shader_info );

            check_error_condition( return render_pipeline(), vulkan_log_errors, not vertex_shader_module, "Failed to create vertex shader module from shader" );
            check_error_condition( return render_pipeline(), vulkan_log_errors, not fragment_shader_module, "Failed to create vertex shader module from shader" );

            // Shader stage info
            vk::PipelineShaderStageCreateInfo vertex_shader_stage_info(
                {},                               // flags
                vk::ShaderStageFlagBits::eVertex, // stage
                *vertex_shader_module,            // module
                "main",                           // pName
                nullptr                           // pSpecializationInfo
            );
            vk::PipelineShaderStageCreateInfo fragment_shader_stage_info(
                {},                                 // flags
                vk::ShaderStageFlagBits::eFragment, // stage
                *fragment_shader_module,            // module
                "main",                             // pName
                nullptr                             // pSpecializationInfo
            );
            vk::PipelineShaderStageCreateInfo shader_stages[] = { vertex_shader_stage_info, fragment_shader_stage_info };

            // Vertex input info (currently standardized for only one vertex type mesh::vertex)
            let vertex_description = get_vertex_binding_description<mesh::vertex>();

            vk::PipelineVertexInputStateCreateInfo vertex_input_info(
                {},                                       // flags
                1,                                        // vertexBindingDescriptionCount
                &vertex_description.input,                // pVertexBindingDescriptions
                vertex_description.input_attribute_count, // vertexAttributeDescriptionCount
                vertex_description.input_attributes       // pVertexAttributeDescriptions
            );
            vk::PipelineInputAssemblyStateCreateInfo input_assembly(
                {},                                   // flags
                vk::PrimitiveTopology::eTriangleList, // topology
                false                                 // primitiveRestartEnable
            );

            // Viewport state (with dummy info since this will be overwritten by dynamic state)
            let viewport_size = uint2();
            vk::Viewport viewport(
                0.0,             // x
                0.0,             // y
                viewport_size.x, // width
                viewport_size.y, // height
                0.0,             // minDepth
                1.0              // maxDepth
            );
            vk::Rect2D scissor(
                vk::Offset2D( 0, 0 ),                            // offset
                vk::Extent2D( viewport_size.x, viewport_size.y ) // extent
            );
            vk::PipelineViewportStateCreateInfo viewport_state(
                {},        // flags
                1,         // viewportCount
                &viewport, // pViewports
                1,         // scissorCount
                &scissor   // pScissors
            );

            // Rasterization state
            vk::PipelineRasterizationStateCreateInfo rasterizer(
                {},                          // flags
                false,                       // depthClampEnable
                false,                       // rasterizerDiscardEnable
                vk::PolygonMode::eFill,      // polygonMode
                vk::CullModeFlagBits::eBack, // cullMode
                vk::FrontFace::eClockwise,   // frontFace
                false,                       // depthBiasEnable
                0.0,                         // depthBiasConstantFactor
                0.0,                         // depthBiasClamp
                0.0,                         // depthBiasSlopeFactor
                1.0                          // lineWidth
            );

            vk::PipelineMultisampleStateCreateInfo multisampling(
                {},                          // flags
                vk::SampleCountFlagBits::e1, // rasterizationSamples
                false,                       // sampleShadingEnable
                1.0,                         // minSampleShading
                nullptr,                     // pSampleMask
                false,                       // alphaToCoverageEnable
                false                        // alphaToOneEnable
            );

            // Color output state
            // TODO: pull this from material info
            let color_write_all = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
            vk::PipelineColorBlendAttachmentState color_blend_attachment(
                false,                  // blendEnable
                vk::BlendFactor::eOne,  // srcColorBlendFactor
                vk::BlendFactor::eZero, // dstColorBlendFactor
                vk::BlendOp::eAdd,      // colorBlendOp
                vk::BlendFactor::eOne,  // srcAlphaBlendFactor
                vk::BlendFactor::eZero, // dstAlphaBlendFactor
                vk::BlendOp::eAdd,      // alphaBlendOp
                color_write_all         // colorWriteMask
            );

            vk::PipelineColorBlendStateCreateInfo color_blending(
                {},                      // flags
                false,                   // logicOpEnable
                vk::LogicOp::eCopy,      // logicOp
                1,                       // attachmentCount
                &color_blend_attachment, // pAttachments
                { 0.0, 0.0, 0.0, 0.0 }   // blendConstants
            );

            static let pipeline_dynamic_state = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
            vk::PipelineDynamicStateCreateInfo dynamic_state(
                {},                            // flags
                pipeline_dynamic_state.size(), // dynamicStateCount
                pipeline_dynamic_state.begin() // pDynamicStates
            );

            // Create pipeline
            vk::GraphicsPipelineCreateInfo graphics_pipeline_info(
                {},                  // flags
                2,                   // stageCount
                shader_stages,       // pStages
                &vertex_input_info,  // pVertexInputState
                &input_assembly,     // pInputAssemblyState
                nullptr,             // pTessellationState
                &viewport_state,     // pViewportState
                &rasterizer,         // pRasterizationState
                &multisampling,      // pMultisampleState
                nullptr,             // pDepthStencilState
                &color_blending,     // pColorBlendState
                &dynamic_state,      // pDynamicState
                new_pipeline_layout, // layout
                render_pass          // renderPass
                                     // subpass            = 0
                                     // basePipelineHandle = null
                                     // basePipelineIndex  = 0
            );

            new_pipeline = vulkan_device.createGraphicsPipelines( pipeline_cache, { graphics_pipeline_info } )[0];
            check_error_condition( return render_pipeline(), vulkan_log_errors, not new_pipeline, "Failed to create Vulkan pipeline" );
        }

        pipelines.emplace_back( new_pipeline, new_pipeline_layout );
        return pipelines.back();
    }

    void resource_database::free_pipeline( render_pipeline& pipeline )
    {
        let& vulkan_device = device_instance.get_vulkan_device();

        vulkan_device.destroyPipeline( pipeline.get_vulkan_pipeline() );
        vulkan_device.destroyPipelineLayout( pipeline.get_layout() );

        pipeline.invalidate();
    }

    // Definitions of helper functions for pipeline creation
    template <typename T>
    static vertex_description get_vertex_binding_description()
    {
        static let invalid_description = vertex_description{ vk::VertexInputBindingDescription(), 0, nullptr };

        let invalid_vertex_type = true;
        check_error_condition( return invalid_description, vulkan_log_errors, invalid_vertex_type == true, "Failed to get binding description for invalid vertex type" );
    }
    template <>
    static vertex_description get_vertex_binding_description<mesh::vertex>()
    {
        static const vk::VertexInputBindingDescription input(
            0,                             // binding
            mesh::vertex_info.vertex_size, // stride
            vk::VertexInputRate::eVertex   // inputRate
        );

        // clang-format off
        static list<vk::VertexInputAttributeDescription> attributes{
            vk::VertexInputAttributeDescription(  // position
                0,                                // location
                0,                                // binding
                vk::Format::eR32G32B32Sfloat,     // format
                mesh::vertex_info.position_offset // offset
            ),
            vk::VertexInputAttributeDescription(  // normal
                1,                                // location
                0,                                // binding
                vk::Format::eR32G32B32Sfloat,     // format
                mesh::vertex_info.normal_offset   // offset
            ), 
            vk::VertexInputAttributeDescription(  // color
                2,                                // location
                0,                                // binding
                vk::Format::eR32G32B32A32Sfloat,  // format
                mesh::vertex_info.color_offset    // offset
            ), 
            vk::VertexInputAttributeDescription(  // uv
                3,                                // location
                0,                                // binding
                vk::Format::eR32G32Sfloat,        // format
                mesh::vertex_info.uv_offset       // offset
            )
        };
        // clang-format on

        return vertex_description{
            input,                   // input
            attributes.data(),       // input_attributes
            (uint) attributes.size() // input_attribute_count
        };
    }

#pragma endregion resource_database

/* -------------------------------------------------------------------------- */
/*                        Internal Resources Component                        */
/* -------------------------------------------------------------------------- */
#pragma region internal_resources

    internal_resources::internal_resources()
      : saved_indices_version( version_id::invalid() ),  //
        saved_vertices_version( version_id::invalid() ), //
        saved_material_version( version_id::invalid() )  //
    {}
    internal_resources::~internal_resources() {}

    void internal_resources::update_mesh_data( const mesh& source, resource_database& resources )
    {
        // Re-allocate a vertex buffer if the source vertices have changed since the last update
        // note: will always be called for the first update, since the saved version starts invalid
        if ( saved_vertices_version.update_to( source.vertices.get_version() ) )
        {
            vulkan_log_verbose.print( "Updating vulkan data for mesh vertices (\1)", source.get_id() );

            // Release an existing vertex buffer if it has been allocated
            // TODO: re-use the same buffer and copy new data if the number of elements stays the same
            if ( vertices.is_valid() )
            {
                resources.free_vertex_buffer( vertices );
            }
            vertices     = resources.create_vertex_buffer( source.vertices.get_data() );
            vertex_count = source.vertices.get_data().size();
        }

        // Re-allocate an index buffer if the source indices have changed since the last update
        // note: will always be called for the first update, since the saved version starts invalid
        if ( saved_indices_version.update_to( source.indices.get_version() ) )
        {
            vulkan_log_verbose.print( "Updating vulkan data for mesh indices (\1)", source.get_id() );

            // Release an existing index buffer if it has been allocated
            // TODO: re-use the same buffer and copy new data if the number of elements stays the same
            if ( indices.is_valid() )
            {
                resources.free_index_buffer( indices );
            }
            indices     = resources.create_index_buffer( source.indices.get_data() );
            index_count = source.indices.get_data().size();
        }
    }

    void internal_resources::update_material_data( const material& source, resource_database& resources, const vk::RenderPass& render_pass )
    {
        // Re-create a pipeline if the source material has changed since the last update
        // note: will always be called for the first update, since the saved version starts invalid
        if ( saved_material_version.update_to( source.get_version() ) )
        {
            vulkan_log_verbose.print( "Updating vulkan data for material (\1)", source.get_id() );

            // Release an existing pipeline if it has been created
            if ( pipeline.is_valid() )
            {
                resources.free_pipeline( pipeline );
            }
            pipeline = resources.create_pipeline( source.get_vertex_shader(), source.get_fragment_shader(), render_pass );
        }
    }

    void internal_resources::release( resource_database& resources )
    {
        if ( vertices.is_valid() )
        {
            resources.free_vertex_buffer( vertices );
        }
        if ( indices.is_valid() )
        {
            resources.free_index_buffer( indices );
        }
        if ( pipeline.is_valid() )
        {
            resources.free_pipeline( pipeline );
        }
    }

#pragma endregion internal_resources
} // namespace rnjin::graphics::vulkan