#ifndef __CVKMEM_BLOCK_ALLOCATOR_H_
#define __CVKMEM_BLOCK_ALLOCATOR_H_
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    namespace nalloc
    {
        static constexpr u32 NUM_TOP_BINS  = 32;
        static constexpr u32 BINS_PER_LEAF = 8;
        static constexpr u32 NUM_LEAF_BINS = NUM_TOP_BINS * BINS_PER_LEAF;

        struct allocation_t
        {
            static constexpr u32 NO_SPACE = 0xffffffff;

            const u32 offset;
            const u32 size;
        };

        struct storage_report_t
        {
            u32 totalFreeSpace    = 0;
            u32 largestFreeRegion = 0;
            u32 numberOfBins      = 0;
            u32 numberOfUsedBins  = 0;
        };

        struct bin_report_t
        {
            u32 size  = 0;
            u32 count = 0;
        };

        class block_allocator_t
        {
        public:
            block_allocator_t();
            block_allocator_t(block_allocator_t&& other);
            ~block_allocator_t();

            // TODO Pass alloc_t* instead of having the block allocator use new/delete
            void init(u32 size, u32 maxAllocs = 128 * 1024);
            void destroy();

            allocation_t* allocate(u32 size);
            void          free(allocation_t* allocation);
            void          storageReport(storage_report_t& report) const;
            void          storageBinState(u32 binIndex, bin_report_t& binState) const;

            struct node_t;
            struct context_t;

        private:
            context_t* m_context;
        };
    }  // namespace nalloc
}  // namespace ncore

#endif  // __CVKMEM_BLOCK_ALLOCATOR_H_
