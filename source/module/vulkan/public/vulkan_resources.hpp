/* *** ** *** ** *** ** *** *
 * Part of rnjin            *
 * (c) Rajin Shankar, 2019  *
 *        rajinshankar.com  *
 * *** ** *** ** *** ** *** */

#pragma once

#include <vulkan/vulkan.hpp>

#include "vulkan_device.hpp"

#include "ecs.hpp"
#include "visual_ecs.hpp"

namespace rnjin::graphics::vulkan
{
    /* -------------------------------------------------------------------------- */
    /*                              Buffer Allocation                             */
    /* -------------------------------------------------------------------------- */

    struct buffer_allocation
    {
        public: // methods
        buffer_allocation();
        buffer_allocation( vk::DeviceSize offset, vk::DeviceSize size, vk::Buffer* buffer );

        public: // accessors
        let get_offset get_value( offset );
        let get_size get_value( size );
        let is_valid get_value( size > 0 && buffer != nullptr );
        let& get_buffer get_value( *buffer );

        private: // members
        vk::DeviceSize offset;
        vk::DeviceSize size;
        vk::Buffer* buffer;

        friend class buffer_allocator;
    };


    /* -------------------------------------------------------------------------- */
    /*                              Buffer Allocator                              */
    /* -------------------------------------------------------------------------- */

    class buffer_allocator
    {
        public: // methods
        buffer_allocator( const device& device_instance, vk::BufferUsageFlags usage_flags, vk::MemoryPropertyFlags memory_property_flags );
        ~buffer_allocator();

        void initialize( vk::DeviceSize total_size );

        buffer_allocation allocate( vk::DeviceSize size );
        void free( buffer_allocation& allocation );

        public: // accessors
        let& get_buffer get_value( buffer );
        let& get_memory get_value( memory );

        private: // members
        const device& device_instance;

        vk::DeviceSize size;
        vk::Buffer buffer;
        vk::DeviceMemory memory;

        vk::BufferUsageFlags usage_flags;
        vk::MemoryPropertyFlags memory_property_flags;

        // A block of free memory, with pointers to the previous and next blocks (in order of offset)
        // note: the next block should never be closer than this block's size; this rule is implicitly enforced in allocate/free
        struct block
        {
            block( vk::DeviceSize size, vk::DeviceSize offset, block* previous, block* next )
              : pass_member( size ),     //
                pass_member( offset ),   //
                pass_member( previous ), //
                pass_member( next )      //
            {}

            vk::DeviceSize size;
            vk::DeviceSize offset;

            block* previous;
            block* next;
        };

        block entry_block;
        vk::DeviceSize available_space;
    };

    /* -------------------------------------------------------------------------- */
    /*                                  Pipeline                                  */
    /* -------------------------------------------------------------------------- */

    class render_pipeline
    {
        public: // methods
        render_pipeline();
        render_pipeline( vk::Pipeline vulkan_pipeline, vk::PipelineLayout layout );
        ~render_pipeline();

        void invalidate();

        public: // accessors
        let& get_vulkan_pipeline get_value( vulkan_pipeline );
        let& get_layout get_value( layout );

        let is_valid get_value( vulkan_pipeline and layout );

        private: // members
        vk::Pipeline vulkan_pipeline;
        vk::PipelineLayout layout;
    };

    /* -------------------------------------------------------------------------- */
    /*                              Resource Database                             */
    /* -------------------------------------------------------------------------- */

    class resource_database
    {
        public: // methods
        resource_database( const device& device_instance );
        ~resource_database();

        void initialize( usize vertex_buffer_space, usize index_buffer_space, usize staging_buffer_space );
        void clean_up();

        buffer_allocation create_vertex_buffer( const list<mesh::vertex>& vertices );
        buffer_allocation create_index_buffer( const list<mesh::index>& indices );
        void free_vertex_buffer( buffer_allocation& allocation );
        void free_index_buffer( buffer_allocation& allocation );

        render_pipeline create_pipeline( const shader& vertex_shader, const shader& fragment_shader, const vk::RenderPass& render_pass );
        void free_pipeline( render_pipeline& pipeline );

        private: // methods
        void write_staging_buffer( const buffer_allocation& staging_buffer_allocation, vk::DeviceSize size, void* source );
        void transfer_staging_buffer( const buffer_allocation& staging_buffer_allocation, const buffer_allocation& target_allocation );
        void free_staging_buffer( buffer_allocation& staging_buffer_allocation );

        private: // members
        const device& device_instance;
        vk::PipelineCache pipeline_cache;
        list<render_pipeline> pipelines;

        // note: this should probably have several 'chunks' of allocators for each use case, instead of just one massive one
        buffer_allocator vertex_buffer_allocator;
        buffer_allocator index_buffer_allocator;
        buffer_allocator staging_buffer_allocator;
    };


    /* -------------------------------------------------------------------------- */
    /*                        Internal Resources Component                        */
    /* -------------------------------------------------------------------------- */

    component_class( internal_resources )
    {
        public: // methods
        internal_resources();
        ~internal_resources();

        void update_mesh_data( const mesh& source, resource_database& resources );
        void update_material_data( const material& source, resource_database& resources, const vk::RenderPass& render_pass );
        void release( resource_database & resource );

        let& get_vertices get_value( vertices );
        let& get_indices get_value( indices );
        let& get_pipeline get_value( pipeline );

        let get_vertex_count get_value( vertex_count );
        let get_index_count get_value( index_count );

        private: // members
        version_id saved_vertices_version;
        version_id saved_indices_version;
        version_id saved_material_version;

        usize vertex_count;
        usize index_count;

        buffer_allocation vertices;
        buffer_allocation indices;
        render_pipeline pipeline;
    };
} // namespace rnjin::graphics::vulkan