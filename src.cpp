#ifndef __PROGTEST__

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <cmath>

using namespace std;

#endif /* __PROGTEST__ */

// --------------------------------------------- BLOCK ---------------------------------------------

struct Block
{
	Block(uint32_t _size) : size(_size), next(nullptr) {}

	// not necessary, simplifies the math
	uint32_t size;
	// next element in a linked list
	Block * next;
};


// --------------------------------------------- VARIABLES ---------------------------------------------

/*
Most of the variables are not needed (they are computable from others)
Storing those values is just a simplification
*/

/// Max number of buddy levels (max size = 16B * 2^31 = 32GiB)
const int MAX_LEVELS = 32;
/// Min size of one buddy system block (in bytes)
const int MIN_SIZE = 16;
/// log2(MIN_SIZE)
const int MIN_SIZE_LOG = 4;

/// linked lists for each level
Block * g_freeBlocks[MAX_LEVELS];
/// Actual number of levels the buddy system is using
int g_levelsNum = 0;

/// address of given memory block
void * g_memStart = nullptr;
/// end of given memory block
void * g_end = nullptr;
/// address where the buddy allocator begins (may not be accessible)
void * g_buddyStart = nullptr;
/// address of the metadata
void * g_metaStart = nullptr;
/// address of the split part of the metadata
void * g_metaSplitStart = nullptr;

/// size of the given block
int g_memSize = 0;
/// size of entire buddy block (>= poolSize)
int g_buddySize = 0;
/// size of the metadata
int g_metaSize = 0;

/// Number of blocks allocated in the pool
int g_blocksPending = 0;

// --------------------------------------------- MATH ---------------------------------------------

class MathBuddy
{
public:
	/// Counts log2 of an integer, rounds to the ceiling
	static int Log2Int(int num)
	{
		int res = 0;
		// is num power of 2? 
		bool perfect = num % 2 == 0 || num == 1;
		// count simple log2(n)
		while (num >>= 1)
		{
			res++;
			perfect = perfect && (num % 2 == 0 || num == 1);
		}
		// add one in case size was not perfect power of 2
		return perfect ? res : res + 1;
	}

	/// Simple power of 2 function for integers
	static int Pow2Int(int num)
	{
		return 1 << num;
	}

	/// Returns true when 'num' is any power of 2, otherwise false
	static bool IsPow2(int num)
	{
		do
		{
			if (num != 1 && num % 2 == 1)
				return false;
		} while (num >>= 1);
		return true;
	}

	/// Returns max size of a block (in bytes) which can begin on specified address
	static int MaxBlockSizeByAddr(int num)
	{
		int div = 1;
		while (num % 2 == 0)
		{
			div <<= 1;
			num >>= 1;
		}
		return div;
	}

	/// Counts linked list's index in the global array based log2 of the memory needed (exponent)
	static int ExpToLevel(int exp)
	{
		return MAX_LEVELS + MIN_SIZE_LOG - exp - 1;
	}

	/// Counts linked list's index in the global array based on the memory needed
	static int ListIndex(int size)
	{
		int exp = Log2Int(size);
		return ExpToLevel(exp);
	}

	/// Gets index of a left child a buddy tree
	static int ChildIndex(int index)
	{
		return ((index + 1) * 2) - 1;
	}

	/// Counts number of levels the buddy allocator will use based on the allocated block
	/// Asumes given memory block is (much) larger than MIN_SIZE
	static int LevelsNeeded(int size)
	{
		return Log2Int(size / MIN_SIZE) + 1;
	}

	/// Count size of one block (in bytes) on specified buddy level
	static int LevelToSize(int level)
	{
		return Pow2Int(MAX_LEVELS + MIN_SIZE_LOG - level - 1);
	}

	/// Counts level of a block based on its size 
	static int SizeToLevel(int size)
	{
		return MAX_LEVELS + MIN_SIZE_LOG - Log2Int(size) - 1;
	}

	/// Counts max possible number of blocks on specified level
	static int BlocksNumAtLevel(int level)
	{
		return Pow2Int(level);
	}

	/// Counts index of a first (theoretical) block on specified level
	static int IndexOfLevel(int level)
	{
		return Pow2Int(level - MAX_LEVELS + g_levelsNum) - 1;
	}

	/// Counts index of a block within specified level
	/// Asumes the block pointer is valid
	/// When the level is not specified, actual level of the block is used
	static int IndexWithinLevel(Block * block, int level = -1)
	{
		if (level == -1)
			level = SizeToLevel(block->size);
		int offset = ((uint8_t *)block - (uint8_t *)g_buddyStart);
		return offset / LevelToSize(level);
	}

	/// Counts unique identifier of a block on specified level
	/// The block doesn't have to exist (can be either split or part of a larger block)
	/// When the level is not specified, actual level of the block is used
	static int IndexGlobal(Block * block, int level = -1)
	{
		if (level == -1)
			level = SizeToLevel(block->size);
		return IndexOfLevel(level) + IndexWithinLevel(block, level);
	}

	/// Returns level of a block with specified global index
	static int IndexGlobalToLevel(int index)
	{
		int lg = (int)log2(index + 1);
		return MAX_LEVELS - g_levelsNum + lg;
	}

	/// Finds block's buddy, returns nullptr when the buddy is off the bounds
	/// When the level is not specified, actual level of the block is used
	static Block * FindBuddy(Block * block, int level = -1)
	{
		if (level == -1)
			level = SizeToLevel(block->size);
		int index = IndexWithinLevel(block, level);

		if (index % 2 == 0)
		{
			// buddy's on the right
			uint8_t * addr = (uint8_t *)block + block->size;
			if (addr + block->size > (uint8_t *)g_end)
				// off bounds
				return nullptr;
			return (Block *)(addr);
		}
		else
		{
			// buddy's on the left 
			uint8_t * addr = (uint8_t *)block - block->size;
			if (addr < (uint8_t *)g_memStart)
				// off bounds
				return nullptr;
			return (Block *)(addr);
		}
	}
};


// --------------------------------------------- DEBUG ---------------------------------------------

#ifndef __PROGTEST__

/// Prints info about free block in the memory
void DebugBuddySystemInfo()
{
	printf("\n* DEBUG *\n\n");

	int offset = (int)((uint8_t *)g_metaStart - (uint8_t *)g_buddyStart);
	printf("[METADATA] size: %d, offset: %d, ratio: %.4f\n",
		g_metaSize, offset, offset / (float)g_buddySize);

	offset = (int)((uint8_t *)g_memStart - (uint8_t *)g_buddyStart);
	printf("[BUDDY SYTSTEM] size: %d, start offset: %d, ratio: %.4f\n",
		g_buddySize, offset, offset / (float)g_buddySize);
	printf("Free memory blocks:\n");
	int sum = 0;
	for (int i = 0; i < MAX_LEVELS; i++)
	{
		int exp = MAX_LEVELS - i + MIN_SIZE_LOG - 1;
		if (i < MAX_LEVELS - g_levelsNum)
			// unused levels
			printf("  [NOT USED] i: %d, pow of %d: ", i, exp);
		else
		{
			// used levels
			printf("  i: %d, pow of %d (%d B): ", i, exp, MathBuddy::LevelToSize(i));
		}

		Block * tmp = g_freeBlocks[i];
		if (!tmp)
			printf("empty");
		// print all blocks
		while (tmp)
		{
			printf("addr: (buddy_rel: %d, mem_rel: %d, addr: %d), index: (global: %d, level: %d), size: %d",
				(int)((uint8_t *)tmp - (uint8_t *)g_buddyStart), (int)((uint8_t *)tmp - (uint8_t *)g_memStart), (int)(long int)tmp,
				MathBuddy::IndexGlobal(tmp, i), MathBuddy::IndexWithinLevel(tmp, i),
				tmp->size);
			sum += tmp->size;
			tmp = tmp->next;
		}
		printf("\n");
	}
	printf("memory left: %d B\n\n", sum);
}

/// Prints both metadata bitmaps 
void DebugBuddySystemMeta(int bytesPerRow = 16)
{
	printf("\n* METADATA DEBUG *\n\n");

	printf("Taken leafs bitmap:\n");
	// fit 32 bits into each row
	int leafsTotal = g_buddySize / MIN_SIZE;
	int columns = (leafsTotal / 8) / bytesPerRow;
	uint8_t * tmp = (uint8_t *)g_metaStart;
	for (int i = 0; i < columns; i++)
	{
		printf("  ");
		for (int j = 0; j < bytesPerRow; j++, tmp++)
			printf("%#04x ", *tmp);
		printf("\n");
	}
	printf("Split nodes bitmap:\n");
	for (int i = 0; i < columns; i++)
	{
		printf("  ");
		for (int j = 0; j < bytesPerRow; j++, tmp++)
			printf("%#04x ", *tmp);
		printf("\n");
	}
	printf("\n");
}

#endif /* __PROGTEST__ */

// --------------------------------------------- FUNCTIONS ---------------------------------------------

/// Gets allocator ready for the next run
void ResetAllocator()
{
	g_blocksPending = 0;
	for (int i = 0; i < MAX_LEVELS; i++)
		g_freeBlocks[i] = nullptr;
	g_levelsNum = 0;
	g_memStart = nullptr;
	g_end = nullptr;
	g_buddyStart = nullptr;
	g_metaStart = nullptr;
	g_metaSplitStart = nullptr;
	g_memSize = 0;
	g_buddySize = 0;
	g_metaSize = 0;
	g_blocksPending = 0;
}

/// Adds free memory block to corresponding linked list (based on the level)
void AddFree(Block * block, int level)
{
	if (!block)
		return;
	// add new block to the beggining of a list
	Block * former = g_freeBlocks[level];
	g_freeBlocks[level] = block;
	block->next = former;
}

/// Tries to remove a block from a corresponding linked list (based on the level)
/// Returns success
bool RemoveFree(Block * block, int level)
{
	// simple deletion from a linked list

	Block* tmp = g_freeBlocks[level];
	if (!tmp)
		// empty list
		return false;
	if (tmp == block)
	{
		// match with first element
		g_freeBlocks[level] = tmp->next;
		return true;
	}

	// go through the list
	while (tmp)
	{
		if (block == tmp->next)
		{
			// delete
			tmp->next = block->next;
			return true;
		}
		// move
		tmp = tmp->next;
	}

	return false;
}

/// Returns a number with last 'n' bits set to 1, rest to 0
uint8_t GetOnes(int n)
{
	uint8_t ones = 0xff;
	return ones >> (8 - n);
}

/// Returns a number with last 'n' bits set to 0, rest to 1
uint8_t GetZeros(int n)
{
	uint8_t ones = 0xff;
	return ones << n;
}

/// Generates a mask with all '0' bits and one '1' bit at 'n'th bit
uint8_t GetOneAt(int n)
{
	return 1 << (7 - n);
}

/// Generates a mask with all '1' bits and one '0' bit at 'n'th bit
uint8_t GetZeroAt(int n)
{
	return ~GetOneAt(n);
}

/// Marks 'numBits' leafs as either taken or free (based on 'asOnes'), staring with the 'startBit'th
void MarkBits(void * container, int startBit, int numBits, bool asOnes)
{
	int firstByte = startBit / 8, firstBit = startBit % 8;
	uint8_t * currentByte = (uint8_t *)container + firstByte;
	// make change within 1 byte
	if (firstBit + numBits < 8)
	{
		uint8_t mask = GetOnes(numBits);
		mask <<= (8 - (firstBit + numBits));
		if (!asOnes)
		{
			// set related bits to 0
			mask = ~mask;
			*currentByte &= mask;
		}
		else
			// set related bits to 1
			*currentByte |= mask;
	}
	// make changes within more bytes
	else
	{
		// set starting byte
		if (asOnes)
			*currentByte |= GetOnes(8 - firstBit);
		else
			*currentByte &= GetZeros(8 - firstBit);
		numBits -= (8 - firstBit);
		currentByte++;
		// set whole bytes
		uint8_t fill = asOnes ? 0xff : 0x00;
		memset(currentByte, fill, numBits / 8);
		numBits %= 8;
		// set last byte affected
		uint8_t mask = GetOnes(numBits);
		mask <<= (8 - numBits);
		if (!asOnes)
		{
			mask = ~mask;
			*currentByte &= mask;
		}
		else
			*currentByte |= mask;
	}
}

/// Marks 'numLeafs' leafs as taken, staring with the 'startLeaf'th
void MarkTaken(int startLeaf, int numLeafs)
{
	MarkBits(g_metaStart, startLeaf, numLeafs, true);
}

/// Marks 'numLeafs' leafs as free, staring with the 'startLeaf'th
void MarkFree(int startLeaf, int numLeafs)
{
	MarkBits(g_metaStart, startLeaf, numLeafs, false);
}

/// Marks block of specified global index as split
void MarkSplit(int index)
{
	if (!g_metaSplitStart)
		return;
	// find related byte
	uint8_t * byte = (uint8_t *)g_metaSplitStart + (index / 8);
	// set related bit to 1
	*byte |= GetOneAt(index % 8);
}

/// Marks block of specified global index as merged
void MarkMerged(int index)
{
	if (!g_metaSplitStart)
		return;
	// find related byte
	uint8_t * byte = (uint8_t *)g_metaSplitStart + (index / 8);
	// set related bit to 0
	*byte &= GetZeroAt(index % 8);;
}

/// Returns whether a block of specified global index is split or not
bool IsSplit(int index)
{
	if (index >= g_buddySize / MIN_SIZE - 1 || !g_metaSplitStart)
		// block is a leaf or the index is invalid
		return false;
	// find related byte
	uint8_t * byte = (uint8_t *)g_metaSplitStart + (index / 8);
	// check related bit's value
	return *byte & GetOneAt(index % 8);
}

/// Returns whether the leaf of specified index (within the leaf level) is taken or not
bool IsLeafTaken(int leafIndex)
{
	if (!g_metaStart)
		return false;
	// find related byte
	uint8_t * byte = (uint8_t *)g_metaStart + (leafIndex / 8);
	// check related bit's value
	return *byte & GetOneAt(leafIndex % 8);
}

/// Returns whether a block of specified global index is being used or not
bool IsTaken(int index)
{
	// find level of the block
	int level = MathBuddy::IndexGlobalToLevel(index);
	// find index of a leaf alligned with the block
	while (level < MAX_LEVELS - 1)
	{
		level++;
		index = MathBuddy::ChildIndex(index);
	}
	// find its index within the leaf level
	int leafIndex = index - MathBuddy::IndexOfLevel(MAX_LEVELS - 1);
	// check whether the leaf is taken or not
	return IsLeafTaken(leafIndex);
}

/// Marks specified block as taken
void MarkAlloc(Block * block, int level)
{
	if (g_metaStart)
	{
		// count block's offset from the begginging
		int offset = (int)((uint8_t *)block - (uint8_t *)g_buddyStart);
		// mark all leafs it covers
		MarkTaken(offset / MIN_SIZE, block->size / MIN_SIZE);
	}
}

/// Initializes buddy system, fills given memory block with empty blocks
void InitBuddySystem()
{
	// count buddy allocator info
	int exp = g_levelsNum + MIN_SIZE_LOG - 1;
	g_buddySize = MathBuddy::Pow2Int(exp);
	g_buddyStart = (void *)((uint8_t *)g_end - g_buddySize);

	// start with a block of max size
	int blockSize = g_buddySize;
	int memLeft = g_memSize;
	int level = MathBuddy::ExpToLevel(exp);

	while (blockSize >= MIN_SIZE)
	{
		// fill the memory whenever a block of current size fits into it
		if (blockSize <= memLeft)
		{
			// create new free block
			Block * block = (Block *)((uint8_t *)g_memStart + (memLeft - blockSize));
			g_freeBlocks[level] = block;
			block->size = blockSize;
			block->next = nullptr;
			memLeft -= blockSize;
		}

		// move one level down
		blockSize /= 2;
		level++;
	}
}

/// Initializes metadata
void InitMeta()
{
	// set all free bits to 0
	int leafsTaken = (int)((uint8_t *)g_memStart - (uint8_t *)g_buddyStart) / MIN_SIZE;
	int leafsTotal = g_buddySize / MIN_SIZE;
	float ratioTaken = (float)leafsTaken / leafsTotal;
	if (leafsTaken > 0)
	{
		// set bits out of the memory block to 1, rest to 0
		int leafsFree = leafsTotal - leafsTaken;

		MarkFree(leafsTaken, leafsFree);
		MarkTaken(0, leafsTaken);
	}
	else
		// set all bits to 0
		MarkFree(0, leafsTotal);

	// mark space taken by the metadata 
	int startLeaf = (int)((uint8_t *)g_metaStart - (uint8_t *)g_buddyStart) / (MIN_SIZE * 8);
	MarkTaken(startLeaf, g_metaSize / MIN_SIZE);

	// mark split nodes
	int bitsSet = 0;
	int numBlocksInLevel = 1;
	float ratio = ratioTaken;
	void * start = (void *)((uint8_t *)g_metaStart + g_metaSize / 2);
	// set metadata level after level
	for (int i = 0; i < g_levelsNum - 1; i++, ratio *= 2, numBlocksInLevel *= 2)
	{
		// set split blocks
		int numSplit = (int)ceil(ratio);
		MarkBits(start, bitsSet, numSplit, true);
		// set merged blocks
		int numMerged = numBlocksInLevel - numSplit;
		MarkBits(start, bitsSet + numSplit, numMerged, false);
		// update
		bitsSet += numBlocksInLevel;
	}
}


/// Tries to allocate buddy block of given level
/// When there is none, tries to split blocks of higher levels to create one
Block * AllocOnLevel(int level)
{
	// required block is bigger than the max block possible
	if (level < (MAX_LEVELS - g_levelsNum))
		return nullptr;

	Block * block = g_freeBlocks[level];
	if (block)
	{
		// use first free block
		g_freeBlocks[level] = block->next;
		return block;
	}
	else
	{
		// no block on this level, split bigger blog in half
		Block * first = AllocOnLevel(level - 1);
		if (!first)
			return nullptr;
		// mark as split
		int index = MathBuddy::IndexGlobal(first, level - 1);
		MarkSplit(index);
		// resize original block
		int size = first->size / 2;
		first->size = size;
		// add new block (unused half of the original one)
		Block * second = (Block *)((uint8_t *)first + size);
		second->size = size;
		AddFree(second, level);

		return first;
	}
}

/// Allocates block of given level
/// Returns nullptr where there is not enough space
Block * BuddyAlloc(int level)
{
	Block * block = AllocOnLevel(level);
	if (block)
		MarkAlloc(block, level);
	return block;
}

/// Tries to free a block on specified address
/// Returns success
bool TryFreeBlock(void * addr)
{
	int offset = (int)((uint8_t *)addr - (uint8_t *)g_buddyStart);
	if (offset % MIN_SIZE != 0)
		// cannot be a block
		return false;
	// find biggest block which is not split
	int size = MathBuddy::MaxBlockSizeByAddr(offset);
	int index = MathBuddy::IndexGlobal((Block *)addr, MathBuddy::SizeToLevel(size));
	// keep trying smaller blocks until the correct one is found
	while (IsSplit(index))
	{
		index = MathBuddy::ChildIndex(index);
		size /= 2;
	}
	if (!IsTaken(index))
		// block is free already
		return false;
	// create free block
	Block * block = (Block *)addr;
	block->size = size;
	// mark as free
	int leafIndex = MathBuddy::IndexWithinLevel(block, MAX_LEVELS - 1);
	MarkFree(leafIndex, size / MIN_SIZE);

	return true;
}

/// Merges a block with its buddy using recursion
/// Returns pointer to resulting block
Block * Merge(Block * block)
{
	Block * buddy = MathBuddy::FindBuddy(block);
	if (!buddy)
		return block;
	// try to remove
	int level = MathBuddy::SizeToLevel(block->size);
	if (RemoveFree(buddy, level))
	{
		// success -> buddy was free, merge
		Block * merged = block < buddy ? block : buddy;
		merged->size *= 2;
		// mark as merged
		int index = MathBuddy::IndexGlobal(merged, level - 1);
		MarkMerged(index);
		// Try to merge with another 
		return Merge(merged);
	}
	// buddy is not free
	return block;
}

// --------------------------------------------- API ---------------------------------------------

void HeapInit(void * memPool, int memSize);
void * HeapAlloc(int size);
bool HeapFree(void * blk);
void HeapDone(int * pendingBlk);

/// Initializes the heap with a memory block of given size
void HeapInit(void * memPool, int memSize)
{
	// clear memory first
	ResetAllocator();
	// cut memory which can't be covered even by a min block 
	g_memSize = (memSize >> MIN_SIZE_LOG) << MIN_SIZE_LOG;
	g_memStart = memPool;
	g_end = (void *)((uint8_t *)g_memStart + g_memSize);

	// init buddy allocator
	g_levelsNum = MathBuddy::LevelsNeeded(memSize);
	InitBuddySystem();

	// count metadata size 
	g_metaSize = MathBuddy::Pow2Int(g_levelsNum - 3);  // needed: 2^levels b = 2^(levels-3) B 
	// allocate metadata space using the allocator
	int index = MathBuddy::SizeToLevel(g_metaSize);
	g_metaStart = BuddyAlloc(index);
	g_metaSplitStart = (void*)((uint8_t *)g_metaStart + g_metaSize / 2);

	InitMeta();
}

/// Allocates memory block of 'size' bytes on the heap
/// Returns pointer to the block
void * HeapAlloc(int size)
{
	int index = MathBuddy::SizeToLevel(size);
	Block * block = BuddyAlloc(index);

	if (!block)
		return nullptr;

	g_blocksPending++;
	return (void *)block;
}

/// Tries to free a memory block
/// Returns success
bool HeapFree(void * blk)
{
	if (blk < g_memStart || blk >= g_end)
		// off bounds
		return false;
	if (blk == g_metaStart)
		// block reserved for the metadata
		return false;
	// try to free
	if (!TryFreeBlock(blk))
		return false;
	// merge new block
	Block* merged = Merge((Block *)blk);
	// add it to corresponding list
	int level = MathBuddy::SizeToLevel(merged->size);
	AddFree(merged, level);

	g_blocksPending--;
	return true;
}

/// Returns number of blocks allocated in the memory 
void HeapDone(int * pendingBlk)
{
	*pendingBlk = g_blocksPending;
}

// --------------------------------------------- TESTING ---------------------------------------------

#ifndef __PROGTEST__

void TestRef()
{
	uint8_t * p0, *p1, *p2, *p3, *p4;
	int pendingBlk;
	// = 3 * 2^20 B = 3 MiB
	static uint8_t memPool[3 * 1048576];

	// simple allocation
	HeapInit(memPool, 2097152);
	assert((p0 = (uint8_t*)HeapAlloc(512000)) != NULL);
	memset(p0, 0, 512000);
	assert((p1 = (uint8_t*)HeapAlloc(511000)) != NULL);
	memset(p1, 0, 511000);
	assert((p2 = (uint8_t*)HeapAlloc(26000)) != NULL);
	memset(p2, 0, 26000);
	HeapDone(&pendingBlk);
	assert(pendingBlk == 3);

	DebugBuddySystemInfo();

	// reallocating after calling heap free
	HeapInit(memPool, 2097152);
	assert((p0 = (uint8_t*)HeapAlloc(1000000)) != NULL);
	memset(p0, 0, 1000000);
	assert((p1 = (uint8_t*)HeapAlloc(250000)) != NULL);
	memset(p1, 0, 250000);
	assert((p2 = (uint8_t*)HeapAlloc(250000)) != NULL);
	memset(p2, 0, 250000);
	assert((p3 = (uint8_t*)HeapAlloc(250000)) != NULL);
	memset(p3, 0, 250000);
	assert((p4 = (uint8_t*)HeapAlloc(50000)) != NULL);
	memset(p4, 0, 50000);
	assert(HeapFree(p2));
	assert(HeapFree(p4));
	assert(HeapFree(p3));
	assert(HeapFree(p1));
	assert((p1 = (uint8_t*)HeapAlloc(500000)) != NULL);
	memset(p1, 0, 500000);
	assert(HeapFree(p0));
	assert(HeapFree(p1));
	HeapDone(&pendingBlk);
	assert(pendingBlk == 0);

	// allocating up to 2.000.000 from 2.359.296, then reallocating 300.000 instead of 500.000
	HeapInit(memPool, 2359296);
	assert((p0 = (uint8_t*)HeapAlloc(1000000)) != NULL);
	memset(p0, 0, 1000000);
	assert((p1 = (uint8_t*)HeapAlloc(500000)) != NULL);
	memset(p1, 0, 500000);
	assert((p2 = (uint8_t*)HeapAlloc(500000)) != NULL);
	memset(p2, 0, 500000);
	assert((p3 = (uint8_t*)HeapAlloc(500000)) == NULL);
	assert(HeapFree(p2));
	assert((p2 = (uint8_t*)HeapAlloc(300000)) != NULL);
	memset(p2, 0, 300000);
	assert(HeapFree(p0));
	assert(HeapFree(p1));
	HeapDone(&pendingBlk);
	assert(pendingBlk == 1);

	// invalid heap free
	HeapInit(memPool, 2359296);
	assert((p0 = (uint8_t*)HeapAlloc(1000000)) != NULL);
	memset(p0, 0, 1000000);
	assert(!HeapFree(p0 + 1000));
	HeapDone(&pendingBlk);
	assert(pendingBlk == 1);
}

int main(void)
{
	TestRef();

	return 0;
}

#endif /* __PROGTEST__ */
