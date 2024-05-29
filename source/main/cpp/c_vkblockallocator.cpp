#include "cvkmem/private/c_vkblockallocator.h"

// block_allocator_t based on Sebastian Aaltonen's Offset block_allocator_t:
// https://github.com/sebbbi/OffsetAllocator/blob/main/offsetAllocator.cpp

namespace ncore
{
    namespace nalloc
    {
#define ASSERT(x)

        static constexpr u32 TOP_BINS_INDEX_SHIFT = 3;
        static constexpr u32 LEAF_BINS_INDEX_MASK = 0x7;

        struct block_allocator_t::context_t
        {
            // 16 bit offsets mode will halve the metadata storage cost
            // But it only supports up to 65536 maximum allocation count
#ifdef USE_16_BIT_NODE_INDICES
            typedef u16 index_t;
#else
            typedef u32 index_t;
#endif

            void init(u32 size, u32 maxAllocs);
            void reset();
            void destroy();

            inline u32 nodeToIdx(node_t* node) const { return (u32)(node - m_nodes); }

            u32     m_size;
            u32     m_maxAllocs;
            u32     m_freeStorage;
            u32     m_usedBinsTop;
            u8      m_usedBins[NUM_TOP_BINS];
            index_t m_binIndices[NUM_LEAF_BINS];
            node_t* m_nodes;
            u32     m_freeNodeIndex;
            node_t* m_freeNodeHead;
            u32     m_freeOffset;
        };

        typedef block_allocator_t::context_t context_t;
        typedef context_t::index_t           index_t;

        context_t::context_t()
            : m_size(0)
            , m_maxAllocs(0)
            , m_freeStorage(0)
            , m_usedBinsTop(0)
            , m_nodes(nullptr)
            , m_freeNodeIndex(0)
            , m_freeNodeHead(nullptr)
            , m_freeOffset(0)
        {
        }

        void context_t::init(u32 size, u32 maxAllocs)
        {
            m_size       = size;
            m_maxAllocs  = maxAllocs;
            m_freeOffset = maxAllocs - 1;
            if (sizeof(index_t) == 2)
            {
                ASSERT(maxAllocs <= 65536);
            }
            reset();
        }

        void context_t::destroy()
        {
            if (m_nodes)
            {
                delete[] m_nodes;
                m_nodes = nullptr;
            }
        }

        void context_t::reset()
        {
            m_freeStorage = 0;
            m_usedBinsTop = 0;
            m_freeOffset  = m_maxAllocs - 1;

            for (u32 i = 0; i < NUM_TOP_BINS; i++)
                m_usedBins[i] = 0;

            for (u32 i = 0; i < NUM_LEAF_BINS; i++)
                m_binIndices[i] = node_t::unused;

            if (m_nodes == nullptr)
                m_nodes = new node_t[m_maxAllocs];

            m_freeNodeIndex = 0;
            m_freeNodeHead  = nullptr;
        }

        static u32  sInsertNodeIntoBin(context_t* ctx, u32 size, u32 offset);
        static void sRemoveNodeFromBin(context_t* ctx, u32 nodeIndex);

        struct block_allocator_t::node_t
        {
            static constexpr index_t unused = 0xffffffff;

            u32     dataOffset  = 0;
            u32     dataSize    = 0;
            index_t binListPrev = unused;
            index_t binListNext = unused;

            void setUsed(bool used) { neighborNext = used ? (neighborNext | 0x80000000) : (neighborNext & 0x7fffffff); }
            bool isUsed() const { return neighborNext & 0x80000000; }

            index_t getNeighborNext() const { return neighborNext & 0x7fffffff; }
            index_t getNeighborPrev() const { return neighborPrev; }

            void setNeighborNext(index_t index) { neighborNext = (neighborNext & 0x80000000) | (index & 0x7fffffff); }
            void setNeighborPrev(index_t index) { neighborPrev = index; }

            index_t neighborPrev = unused;
            index_t neighborNext = unused;
        };

        typedef block_allocator_t::node_t node_t;

        inline u32 lzcnt_nonzero(u32 v)
        {
#ifdef _MSC_VER
            unsigned long retVal;
            _BitScanReverse(&retVal, v);
            return 31 - retVal;
#else
            return __builtin_clz(v);
#endif
        }

        inline u32 tzcnt_nonzero(u32 v)
        {
#ifdef _MSC_VER
            unsigned long retVal;
            _BitScanForward(&retVal, v);
            return retVal;
#else
            return __builtin_ctz(v);
#endif
        }

        namespace SmallFloat
        {
            static constexpr u32 MANTISSA_BITS  = 3;
            static constexpr u32 MANTISSA_VALUE = 1 << MANTISSA_BITS;
            static constexpr u32 MANTISSA_MASK  = MANTISSA_VALUE - 1;

            // Bin sizes follow floating point (exponent + mantissa) distribution (piecewise linear log approx)
            // This ensures that for each size class, the average overhead percentage stays the same
            u32 uintToFloatRoundUp(u32 size)
            {
                u32 exp      = 0;
                u32 mantissa = 0;

                if (size < MANTISSA_VALUE)
                {
                    // Denorm: 0..(MANTISSA_VALUE-1)
                    mantissa = size;
                }
                else
                {
                    // Normalized: Hidden high bit always 1. Not stored. Just like float.
                    u32 leadingZeros  = lzcnt_nonzero(size);
                    u32 highestSetBit = 31 - leadingZeros;

                    u32 mantissaStartBit = highestSetBit - MANTISSA_BITS;
                    exp                  = mantissaStartBit + 1;
                    mantissa             = (size >> mantissaStartBit) & MANTISSA_MASK;

                    u32 lowBitsMask = (1 << mantissaStartBit) - 1;

                    // Round up!
                    if ((size & lowBitsMask) != 0)
                        mantissa++;
                }

                return (exp << MANTISSA_BITS) + mantissa;  // + allows mantissa->exp overflow for round up
            }

            u32 uintToFloatRoundDown(u32 size)
            {
                u32 exp      = 0;
                u32 mantissa = 0;

                if (size < MANTISSA_VALUE)
                {
                    // Denorm: 0..(MANTISSA_VALUE-1)
                    mantissa = size;
                }
                else
                {
                    // Normalized: Hidden high bit always 1. Not stored. Just like float.
                    u32 leadingZeros  = lzcnt_nonzero(size);
                    u32 highestSetBit = 31 - leadingZeros;

                    u32 mantissaStartBit = highestSetBit - MANTISSA_BITS;
                    exp                  = mantissaStartBit + 1;
                    mantissa             = (size >> mantissaStartBit) & MANTISSA_MASK;
                }

                return (exp << MANTISSA_BITS) | mantissa;
            }

            u32 floatToUint(u32 floatValue)
            {
                u32 exponent = floatValue >> MANTISSA_BITS;
                u32 mantissa = floatValue & MANTISSA_MASK;
                if (exponent == 0)
                {
                    // Denorms
                    return mantissa;
                }
                else
                {
                    return (mantissa | MANTISSA_VALUE) << (exponent - 1);
                }
            }
        }  // namespace SmallFloat

        // Utility functions
        u32 findLowestSetBitAfter(u32 bitMask, u32 startBitIndex)
        {
            u32 maskBeforeStartIndex = (1 << startBitIndex) - 1;
            u32 maskAfterStartIndex  = ~maskBeforeStartIndex;
            u32 bitsAfter            = bitMask & maskAfterStartIndex;
            if (bitsAfter == 0)
                return allocation_t::NO_SPACE;
            return tzcnt_nonzero(bitsAfter);
        }

        // block_allocator_t...
        block_allocator_t::block_allocator_t()
            : m_context(nullptr)
        {
        }

        block_allocator_t::block_allocator_t(block_allocator_t&& other)
            : m_context(other.m_context)
        {
            other.m_context = nullptr;
        }

        block_allocator_t::~block_allocator_t() {}

        void block_allocator_t::init(u32 size, u32 maxAllocs)
        {
            ASSERT(!m_context);

            m_context = new context_t();
            m_context->init(size, maxAllocs);
            m_context->reset();

            // Start state: Whole storage as one big node
            // Algorithm will split remainders and push them back as smaller nodes
            sInsertNodeIntoBin(m_context, m_context->m_size, 0);
        }

        void block_allocator_t::destroy()
        {
            ASSERT(m_context);
            m_context->destroy();
            delete m_context;
            m_context = nullptr;
        }

        allocation_t* block_allocator_t::allocate(u32 size)
        {
            // Out of allocations?
            if (m_context->m_freeOffset == 0)
            {
                return nullptr;
            }

            // Round up to bin index to ensure that alloc >= bin
            // Gives us min bin index that fits the size
            u32 minBinIndex = SmallFloat::uintToFloatRoundUp(size);

            u32 minTopBinIndex  = minBinIndex >> TOP_BINS_INDEX_SHIFT;
            u32 minLeafBinIndex = minBinIndex & LEAF_BINS_INDEX_MASK;

            u32 topBinIndex  = minTopBinIndex;
            u32 leafBinIndex = allocation_t::NO_SPACE;

            // If top bin exists, scan its leaf bin. This can fail (NO_SPACE).
            if (m_context->m_usedBinsTop & (1 << topBinIndex))
            {
                leafBinIndex = findLowestSetBitAfter(m_context->m_usedBins[topBinIndex], minLeafBinIndex);
            }

            // If we didn't find space in top bin, we search top bin from +1
            if (leafBinIndex == allocation_t::NO_SPACE)
            {
                topBinIndex = findLowestSetBitAfter(m_context->m_usedBinsTop, minTopBinIndex + 1);

                // Out of space?
                if (topBinIndex == allocation_t::NO_SPACE)
                {
                    return nullptr;
                }

                // All leaf bins here fit the alloc, since the top bin was rounded up. Start leaf search from bit 0.
                // NOTE: This search can't fail since at least one leaf bit was set because the top bit was set.
                leafBinIndex = tzcnt_nonzero(m_context->m_usedBins[topBinIndex]);
            }

            u32 binIndex = (topBinIndex << TOP_BINS_INDEX_SHIFT) | leafBinIndex;

            // Pop the top node of the bin. Bin top = node.next.
            u32 const     nodeIndex     = m_context->m_binIndices[binIndex];
            node_t* const node          = &m_context->m_nodes[nodeIndex];
            u32 const     nodeTotalSize = node->dataSize;
            node->dataSize              = size;
            node->setUsed(true);
            m_context->m_binIndices[binIndex] = node->binListNext;
            if (node->binListNext != node_t::unused)
                m_context->m_nodes[node->binListNext].binListPrev = node_t::unused;
            m_context->m_freeStorage -= nodeTotalSize;
#ifdef DEBUG_VERBOSE
            printf("Free storage: %u (-%u) (allocate)\n", m_freeStorage, nodeTotalSize);
#endif

            // Bin empty?
            if (m_context->m_binIndices[binIndex] == node_t::unused)
            {
                // Remove a leaf bin mask bit
                m_context->m_usedBins[topBinIndex] &= ~(1 << leafBinIndex);

                // All leaf bins empty?
                if (m_context->m_usedBins[topBinIndex] == 0)
                {
                    // Remove a top bin mask bit
                    m_context->m_usedBinsTop &= ~(1 << topBinIndex);
                }
            }

            // Push back reminder N elements to a lower bin
            u32 reminderSize = nodeTotalSize - size;
            if (reminderSize > 0)
            {
                u32 const newNodeIndex = sInsertNodeIntoBin(m_context, reminderSize, node->dataOffset + size);

                // Link nodes next to each other so that we can merge them later if both are free
                // And update the old next neighbor to point to the new node (in middle)
                if (node->getNeighborNext() != node_t::unused)
                    m_context->m_nodes[node->getNeighborNext()].setNeighborPrev(newNodeIndex);
                m_context->m_nodes[newNodeIndex].setNeighborPrev(nodeIndex);
                m_context->m_nodes[newNodeIndex].setNeighborNext(node->getNeighborNext());
                node->setNeighborNext(newNodeIndex);
            }

            return (allocation_t*)node;  //{.offset = node.dataOffset, .metadata = nodeIndex};
        }

        void block_allocator_t::free(allocation_t* allocation)
        {
            // ASSERT(allocation != nullptr);
            if (!m_context->m_nodes)
                return;

            node_t* const node = (node_t*)allocation;

            // Double delete check
            ASSERT(node->used == true);

            // Merge with neighbors...
            u32 offset = node->dataOffset;
            u32 size   = node->dataSize;

            if ((node->getNeighborPrev() != node_t::unused) && (m_context->m_nodes[node->getNeighborPrev()].isUsed() == false))
            {
                // Previous (contiguous) free node: Change offset to previous node offset. Sum sizes
                node_t& prevNode = m_context->m_nodes[node->getNeighborPrev()];
                offset           = prevNode.dataOffset;
                size += prevNode.dataSize;

                // Remove node from the bin linked list and put it in the freelist
                sRemoveNodeFromBin(m_context, node->getNeighborPrev());

                ASSERT(prevNode.neighborNext == nodeIndex);
                node->setNeighborPrev(prevNode.getNeighborPrev());
            }

            if ((node->getNeighborNext() != node_t::unused) && (m_context->m_nodes[node->getNeighborNext()].isUsed() == false))
            {
                // Next (contiguous) free node: Offset remains the same. Sum sizes.
                node_t& nextNode = m_context->m_nodes[node->getNeighborNext()];
                size += nextNode.dataSize;

                // Remove node from the bin linked list and put it in the freelist
                sRemoveNodeFromBin(m_context, node->getNeighborNext());

                ASSERT(nextNode.neighborPrev == nodeIndex);
                node->setNeighborNext(nextNode.getNeighborNext());
            }

            u32 neighborNext = node->getNeighborNext();
            u32 neighborPrev = node->getNeighborPrev();

            // Insert the removed node to freelist
#ifdef DEBUG_VERBOSE
            printf("Putting node %u into freelist[%u] (free)\n", nodeIndex, m_freeOffset + 1);
#endif
            node_t** nodeNext         = (node_t**)node;
            *nodeNext                 = m_context->m_freeNodeHead;
            m_context->m_freeNodeHead = node;

            // Insert the (combined) free node to bin
            u32 combinedNodeIndex = sInsertNodeIntoBin(m_context, size, offset);

            // Connect neighbors with the new combined node
            if (neighborNext != node_t::unused)
            {
                m_context->m_nodes[combinedNodeIndex].setNeighborNext(neighborNext);
                m_context->m_nodes[neighborNext].setNeighborPrev(combinedNodeIndex);
            }
            if (neighborPrev != node_t::unused)
            {
                m_context->m_nodes[combinedNodeIndex].setNeighborPrev(neighborPrev);
                m_context->m_nodes[neighborPrev].setNeighborNext(combinedNodeIndex);
            }
        }

        u32 sInsertNodeIntoBin(context_t* ctx, u32 size, u32 dataOffset)
        {
            // Round down to bin index to ensure that bin >= alloc
            u32 binIndex = SmallFloat::uintToFloatRoundDown(size);

            u32 topBinIndex  = binIndex >> TOP_BINS_INDEX_SHIFT;
            u32 leafBinIndex = binIndex & LEAF_BINS_INDEX_MASK;

            // Bin was empty before?
            if (ctx->m_binIndices[binIndex] == node_t::unused)
            {
                // Set bin mask bits
                ctx->m_usedBins[topBinIndex] |= 1 << leafBinIndex;
                ctx->m_usedBinsTop |= 1 << topBinIndex;
            }

            // Take a freelist node and insert on top of the bin linked list (next = old top)
            u32 topNodeIndex = ctx->m_binIndices[binIndex];

            u32 const nodeIndex = ctx->nodeToIdx(ctx->m_freeNodeHead);
            ctx->m_freeNodeHead = *((node_t**)ctx->m_freeNodeHead);

#ifdef DEBUG_VERBOSE
            printf("Getting node %u from freelist[%u]\n", nodeIndex, m_freeOffset + 1);
#endif
            ctx->m_nodes[nodeIndex] = {.dataOffset = dataOffset, .dataSize = size, .binListNext = topNodeIndex, .binListPrev = node_t::unused};
            if (topNodeIndex != node_t::unused)
                ctx->m_nodes[topNodeIndex].binListPrev = nodeIndex;
            ctx->m_binIndices[binIndex] = nodeIndex;

            ctx->m_freeStorage += size;
#ifdef DEBUG_VERBOSE
            printf("Free storconst age: %u (+%u) (sInsertNodeIntoBin)\n", m_freeStorage, size);
#endif

            return nodeIndex;
        }

        void sRemoveNodeFromBin(context_t* ctx, u32 nodeIndex)
        {
            node_t* const node = &ctx->m_nodes[nodeIndex];

            if (node->binListPrev != node_t::unused)
            {
                // Easy case: We have previous node-> Just remove this node from the middle of the list.
                ctx->m_nodes[node->binListPrev].binListNext = node->binListNext;
                if (node->binListNext != node_t::unused)
                    ctx->m_nodes[node->binListNext].binListPrev = node->binListPrev;
            }
            else
            {
                // Hard case: We are the first node in a bin. Find the bin.

                // Round down to bin index to ensure that bin >= alloc
                u32 const binIndex = SmallFloat::uintToFloatRoundDown(node->dataSize);

                u32 const topBinIndex  = binIndex >> TOP_BINS_INDEX_SHIFT;
                u32 const leafBinIndex = binIndex & LEAF_BINS_INDEX_MASK;

                ctx->m_binIndices[binIndex] = node->binListNext;
                if (node->binListNext != node_t::unused)
                    ctx->m_nodes[node->binListNext].binListPrev = node_t::unused;

                // Bin empty?
                if (ctx->m_binIndices[binIndex] == node_t::unused)
                {
                    // Remove a leaf bin mask bit
                    ctx->m_usedBins[topBinIndex] &= ~(1 << leafBinIndex);

                    // All leaf bins empty?
                    if (ctx->m_usedBins[topBinIndex] == 0)
                    {
                        // Remove a top bin mask bit
                        ctx->m_usedBinsTop &= ~(1 << topBinIndex);
                    }
                }
            }

            // Insert the node to freelist
#ifdef DEBUG_VERBOSE
            printf("Putting node %u into freelist[%u] (sRemoveNodeFromBin)\n", nodeIndex, m_freeOffset + 1);
#endif
            // ctx->m_freeNodes[++ctx->m_freeOffset] = nodeIndex;
            *((node_t**)node)   = ctx->m_freeNodeHead;
            ctx->m_freeNodeHead = node;

            ctx->m_freeStorage -= node->dataSize;
#ifdef DEBUG_VERBOSE
            printf("Free storage: %u (-%u) (sRemoveNodeFromBin)\n", m_freeStorage, node->dataSize);
#endif
        }

        void block_allocator_t::storageReport(storage_report_t& report) const
        {
            u32 largestFreeRegion = 0;
            u32 freeStorage       = 0;

            // Out of allocations? -> Zero free space
            if (m_context->m_freeOffset > 0)
            {
                freeStorage = m_context->m_freeStorage;
                if (m_context->m_usedBinsTop)
                {
                    u32 topBinIndex   = 31 - lzcnt_nonzero(m_context->m_usedBinsTop);
                    u32 leafBinIndex  = 31 - lzcnt_nonzero(m_context->m_usedBins[topBinIndex]);
                    largestFreeRegion = SmallFloat::floatToUint((topBinIndex << TOP_BINS_INDEX_SHIFT) | leafBinIndex);
                    ASSERT(freeStorage >= largestFreeRegion);
                }
            }

            report                  = {.totalFreeSpace = freeStorage, .largestFreeRegion = largestFreeRegion};
            report.numberOfBins     = NUM_LEAF_BINS;
            report.numberOfUsedBins = 0;
            for (u32 i = 0; i < NUM_LEAF_BINS; i++)
                report.numberOfUsedBins += (m_context->m_binIndices[i] != node_t::unused) ? 1 : 0;
        }

        void block_allocator_t::storageBinState(u32 binIndex, bin_report_t& binState) const
        {
            if (binIndex >= NUM_LEAF_BINS)
            {
                binState = {.size = 0, .count = 0};
                return;
            }

            u32 count     = 0;
            u32 nodeIndex = m_context->m_binIndices[binIndex];
            while (nodeIndex != node_t::unused)
            {
                nodeIndex = m_context->m_nodes[nodeIndex].binListNext;
                count++;
            }
            binState = {.size = SmallFloat::floatToUint(binIndex), .count = count};
        }
    }

}  // namespace nalloc
}  // namespace ncore
