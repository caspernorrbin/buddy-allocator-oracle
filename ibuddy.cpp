#include "ibuddy.hpp"
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sys/mman.h>

#ifdef DEBUG
#define BUDDY_DBG(x) std::cout << x << std::endl;
#else
#define BUDDY_DBG(x)                                                           \
  do {                                                                         \
  } while (0)
#endif

template class IBuddyAllocator<IBuddyConfig<4, 27, 1, 0>>;
template class IBuddyAllocator<IBuddyConfig<4, 25, 1, 0>>;
template class IBuddyAllocator<IBuddyConfig<4, 23, 1, 0>>;
template class IBuddyAllocator<IBuddyConfig<4, 21, 1, 0>>;
template class IBuddyAllocator<IBuddyConfig<4, 10, 1, 0>>;
template class IBuddyAllocator<IBuddyConfig<4, 10, 1, 4>>;
template class IBuddyAllocator<IBuddyConfig<4, 8, 1, 0>>;
template class IBuddyAllocator<IBuddyConfig<4, 8, 1, 4>>;
template class IBuddyAllocator<IBuddyConfig<4, 8, 1, 8>>;

namespace {
bool list_empty(double_link *head) { return head->next == head; }

void list_remove(double_link *node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
  node->prev = node;
  node->next = node;
}

void push_back(double_link *head, double_link *node) {
  BUDDY_DBG("pushing back " << node);
  node->prev = head->prev;
  node->next = head;
  head->prev->next = node;
  head->prev = node;
}

double_link *pop_first(double_link *head) {
  if (list_empty(head)) {
    return nullptr;
  }

  double_link *first = head->next;
  first->next->prev = head;
  head->next = first->next;

  first->prev = first;
  first->next = first;
  return first;
}

bool bit_is_set(const unsigned char *bitmap, int index) {
  return static_cast<bool>(bitmap[index / 8] &
                           (1U << (static_cast<unsigned int>(index) % 8)));
}

void set_bit(unsigned char *bitmap, int index) {
  bitmap[index / 8] |= (1U << (static_cast<unsigned int>(index) % 8));
}

void clear_bit(unsigned char *bitmap, int index) {
  bitmap[index / 8] &= ~(1U << (static_cast<unsigned int>(index) % 8));
}

void flip_bit(unsigned char *bitmap, int index) {
  bitmap[index / 8] ^= (1U << (static_cast<unsigned int>(index) % 8));
}

int map_index(int index) {
  // if (index == 0) {
  //   return 0;
  // } else {
  //   return (index - 1) / 2 + 1;
  // }
  return (index - 1) / 2;
}

size_t round_up_pow2(size_t size) {
  if (size == 0) {
    return 1;
  }

  size--;
  size |= size >> 1U;
  size |= size >> 2U;
  size |= size >> 4U;
  size |= size >> 8U;
  size |= size >> 16U;
  size++;

  return size;
}
} // namespace

// Returns the size of a block at the given level
template <typename Config>
unsigned int IBuddyAllocator<Config>::size_of_level(uint8_t level) {
  return _totalSize >> level;
}

template <typename Config>
unsigned int IBuddyAllocator<Config>::index_in_level(uintptr_t ptr,
                                                     uint8_t level) {
  return (ptr - _start) / size_of_level(level);
}

template <typename Config>
unsigned int IBuddyAllocator<Config>::index_of_level(uint8_t level) {
  return (1U << level) - 1;
}

template <typename Config>
unsigned int IBuddyAllocator<Config>::block_index(uintptr_t ptr,
                                                  uint8_t level) {
  return index_of_level(level) + index_in_level(ptr, level);
}

template <typename Config>
unsigned int IBuddyAllocator<Config>::buddy_index(uintptr_t ptr,
                                                  uint8_t level) {
  int block_idx = block_index(ptr, level);
  if (block_idx % 2 == 0) {
    return block_idx - 1;
  }
  return block_idx + 1;
  // return (block_idx + 1) ^ 1;
}

template <typename Config>
uint8_t IBuddyAllocator<Config>::get_level(uintptr_t ptr) {
  if (!_sizeMapEnabled) {
    int index = (ptr - _start) / _minSize;

    if (_sizeBits == 8) {
      return _sizeMap[index];
    }

    const int byteIndex = index / 2;
    const int bitOffset = (index % 2) * 4;

    BUDDY_DBG("getting level at " << index << " byte index: " << byteIndex
                                  << " bit offset: " << bitOffset);

    return (_sizeMap[byteIndex] >> bitOffset) & 0xF;
  }

  for (uint8_t i = _numLevels - 1; i > 0; i--) {
    BUDDY_DBG("block_index: " << block_index(ptr, i - 1));
    if (bit_is_set(_sizeMap, block_index(ptr, i - 1))) {
      BUDDY_DBG("level is: " << i);
      return i;
    }
  }
  return 0;
}

// Returns the number of blocks needed to fill the given size
template <typename Config>
unsigned int IBuddyAllocator<Config>::num_blocks(size_t size, uint8_t level) {
  return size / size_of_level(level) + (size % size_of_level(level) != 0);
}

// Returns the buddy of the given block
template <typename Config>
uintptr_t IBuddyAllocator<Config>::get_buddy(uintptr_t ptr, uint8_t level) {
  if (level == 0) {
    return ptr;
  }
  // int block_idx = block_index(p, level);
  int buddy_idx = buddy_index(ptr, level);
  uintptr_t buddy =
      (_start + size_of_level(level) * (buddy_idx - index_of_level(level)));
  return buddy;
}

// Aligns the given pointer to the left-most pointer of the block
template <typename Config>
uintptr_t IBuddyAllocator<Config>::align_left(uintptr_t ptr, uint8_t level) {
  return (_start + size_of_level(level) *
                       (block_index(ptr, level) - index_of_level(level)));
}

template <typename Config>
IBuddyAllocator<Config>::IBuddyAllocator(void *start, int lazyThreshold,
                                         bool startFull) {
  static_assert(Config::numLevels > 0,
                "Number of levels must be greater than 0");
  static_assert(Config::minBlockSizeLog2 >= 4,
                "Minimum block size must be greater than or equal to 4");
  static_assert(Config::maxBlockSizeLog2 > Config::minBlockSizeLog2,
                "Maximum block size must be greater than minimum block size");
  static_assert(Config::sizeBits == 0 || Config::sizeBits == 4 ||
                    Config::sizeBits == 8,
                "Size bits must be 0, 4, or 8");
  static_assert(!(Config::sizeBits == 4 &&
                  Config::maxBlockSizeLog2 - Config::minBlockSizeLog2 > 16),
                "Combination of sizeBits = 4 and maxBlockSizeLog2 - "
                "minBlockSizeLog2 > 16 is not allowed");

  if (start == nullptr) {
    start = mmap(nullptr, (Config::numRegions * Config::maxBlockSize),
                 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (start == MAP_FAILED) {
      BUDDY_DBG("mmap failed");
      std::terminate();
    }
  }

  _start = reinterpret_cast<uintptr_t>(start);

  _lazyThreshold = lazyThreshold;
  _totalSize = Config::numRegions * Config::maxBlockSize;

  // Initialize bitmaps
  for (int i = 0; i < Config::allocedBitmapSize; i++) {
    _freeBlocks[i] = startFull ? 0x0 : 0x55; // 0x55 = 01010101
  }

  if (Config::sizeBits == 0) {
    if (startFull) {
      for (int i = 0; i < Config::sizeBitmapSize; i++) {
        _sizeMap[i] = 0xFF; // 11111111
      }
    } else {
      for (int i = 0; i < Config::sizeBitmapSize; i++) {
        _sizeMap[i] = 0x00; // 00000000
      }
    }
  }

  // Initialize free lists
  for (int i = 0; i < _numLevels; i++) {
    _freeList[i] = {&_freeList[i], &_freeList[i]};
  }

  if (!startFull) {
    auto *start_link = static_cast<double_link *>(start);

    // Insert blocks into the free lists
    for (uint8_t lvl = _numLevels - 1; lvl > 0; lvl--) {
      for (uintptr_t i = _start + (1U << static_cast<unsigned int>(
                                       _maxBlockSizeLog2 - lvl));
           i < _start + _totalSize;
           i += (2U << static_cast<unsigned int>(_maxBlockSizeLog2 - lvl))) {
        push_back(&_freeList[lvl], reinterpret_cast<double_link *>(i));
      }
    }
    push_back(&_freeList[0], start_link);

    // Initialize
    start_link->prev = &_freeList[0];
    start_link->next = &_freeList[0];
  }

  _lazyList = {&_lazyList, &_lazyList};
}

// Creates a buddy allocator at the given address
template <typename Config>
IBuddyAllocator<Config> *
IBuddyAllocator<Config>::create(void *addr, void *start, int lazyThreshold,
                                bool startFull) {
  if (addr == nullptr) {
    addr = mmap(nullptr, sizeof(IBuddyAllocator), PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (addr == MAP_FAILED) {
      BUDDY_DBG("mmap failed");
      return nullptr;
    }
  }

  return new (addr) IBuddyAllocator(start, lazyThreshold, startFull);
}

// Marks blocks as split above the given level
template <typename Config>
void IBuddyAllocator<Config>::split_bits(uintptr_t ptr, uint8_t level_start,
                                         uint8_t level_end) {
  BUDDY_DBG("splitting bits from " << level_start << " to " << level_end);
  for (uint8_t i = level_start; i < level_end && i < _numLevels - 1; i++) {
    BUDDY_DBG("splitting bit at " << block_index(ptr, i));
    set_bit(_sizeMap, block_index(ptr, i));
  }
}

template <typename Config>
void IBuddyAllocator<Config>::set_level(uintptr_t ptr, uint8_t level) {
  const unsigned int index = (ptr - _start) / _minSize;
  if (_sizeBits == 8) {
    _sizeMap[index] = level;
    return;
  }

  const unsigned int byteIndex = index / 2;
  const unsigned int bitOffset = (index % 2) * 4;
  BUDDY_DBG("setting level at " << index << " byte index: " << byteIndex
                                << " bit offset: " << bitOffset);

  _sizeMap[byteIndex] &=
      ~(0xFU << bitOffset); // Clear the bits for the current level
  // _sizeMap[byteIndex] &=
  // ~(0xF << bitOffset); // Clear the bits for the current level
  _sizeMap[byteIndex] |= (level << bitOffset); // Set the bits for the new level
}

// Returns the size of the allocated block
template <typename Config>
size_t IBuddyAllocator<Config>::get_alloc_size(uintptr_t ptr) {
  return size_of_level(get_level(ptr));
}

// Allocates a block of memory of the given size
template <typename Config>
void *IBuddyAllocator<Config>::allocate(size_t totalSize) {
  // Allocation too large
  if (totalSize > static_cast<size_t>(_maxSize)) {
    BUDDY_DBG("Requested size is too large");
    return nullptr;
  }

  // Allocate from lazy list if possible
  if (_lazyListSize > 0 && totalSize <= static_cast<size_t>(_minSize)) {
    BUDDY_DBG("Allocating from lazy list");
    void *block = pop_first(&_lazyList);
    BUDDY_DBG("allocated block " << block_index(block, _numLevels - 1) << " at "
                                 << block - _start
                                 << " level: " << _numLevels - 1);
    _lazyListSize--;

    return block;
  }

  // Move down if the current level has been exhausted
  while (list_empty(&_freeList[_topLevel]) && _topLevel < _numLevels - 1) {
    _topLevel++;
  }

  uint8_t level = _topLevel;
  BUDDY_DBG("at level: " << level
                         << " with block size: " << size_of_level(level));

  // No free block large enough available
  if (level == _numLevels || list_empty(&_freeList[level]) ||
      static_cast<size_t>(size_of_level(level)) < totalSize) {
    BUDDY_DBG("No free blocks available");
    return nullptr;
  }

  // Get the first free block
  auto block = reinterpret_cast<uintptr_t>(pop_first(&_freeList[level]));

  int block_level = find_smallest_block_level(totalSize);

  // Multiple blocks needed, align the block for its level
  uintptr_t block_left = align_left(block, block_level);
  BUDDY_DBG("aligned block " << block_index(block_left, level) << " at "
                             << block_left - _start << " level: " << level);

  // Mark above blocks as split
  if (_sizeMapEnabled) {
    split_bits(block, level, block_level);
  } else {
    set_level(block_left, block_level);
  }

  // Mark the block as allocated
  clear_bit(_freeBlocks, block_index(block, level));
  BUDDY_DBG("cleared block " << block_index(block, level) << " at "
                             << block - _start << " level: " << level);

  // Can fit in one block
  if (totalSize <= static_cast<size_t>(_minSize)) {
    return reinterpret_cast<void *>(block);
  }

  level = block_level;

  // Clear all smaller levels
  int start_level = find_smallest_block_level(totalSize);
  size_t new_size = round_up_pow2(totalSize);

  BUDDY_DBG("STARTING LEVEL: " << start_level << " SIZE: " << totalSize);

  for (int i = start_level + 1; i < _numLevels; i++) {
    unsigned int start_block_idx = block_index(block_left, i);
    for (unsigned int j = start_block_idx;
         j < start_block_idx + num_blocks(new_size, i); j++) {
      BUDDY_DBG("clearing block " << j << " level: " << i);
      clear_bit(_freeBlocks, j);
    }
  }

  // Clear free list
  for (uintptr_t i = block_left; i < block_left + new_size;
       i += size_of_level(_numLevels - 1)) {
    if (i != block) {
      BUDDY_DBG("clearing free list at " << i - _start);
      list_remove(reinterpret_cast<double_link *>(i));
    }
  }

  return reinterpret_cast<void *>(block_left);
}

// Deallocates a single (smallest) block of memory
template <typename Config>
void IBuddyAllocator<Config>::deallocate_single(uintptr_t ptr) {

  uint8_t level = _numLevels - 1;
  int block_idx = block_index(ptr, level);
  int buddy_idx = buddy_index(ptr, level);

  BUDDY_DBG("block index: " << block_idx << ", buddy index: " << buddy_idx
                            << ", is set: "
                            << bit_is_set(_freeBlocks, buddy_idx));

  // While the buddy is free, go up a level
  while (bit_is_set(_freeBlocks, buddy_idx) && level > 0) {
    level--;
    block_idx = block_index(ptr, level);
    buddy_idx = buddy_index(ptr, level);
    BUDDY_DBG("new block index: " << block_idx << " buddy index: " << buddy_idx
                                  << " level: " << level);

    if (_sizeMapEnabled) {
      clear_bit(_sizeMap, block_idx);
      BUDDY_DBG("clearing split_idx " << block_idx);
    }
  }

  // Mark the block as free and insert it into the free list
  push_back(&_freeList[level], reinterpret_cast<double_link *>(ptr));
  set_bit(_freeBlocks, block_index(ptr, level));
  BUDDY_DBG("inserting " << ptr - _start << " at level: " << level
                         << " marking bit: " << block_index(ptr, level)
                         << " as free");

  // Set the level of the topmost free block
  if (level < _topLevel) {
    _topLevel = level;
  }
}

// Deallocates a range of blocks of memory as if they were the smallest block
template <typename Config>
void IBuddyAllocator<Config>::deallocate_range(void *ptr, size_t size) {
  const auto start = reinterpret_cast<uintptr_t>(ptr);
  for (uintptr_t i = start; i < start + size;
       i += size_of_level(_numLevels - 1)) {

    BUDDY_DBG("deallocating block at " << i - _start);
    deallocate_single(i);
  }
}

// Deallocates a block of memory of the given size
template <typename Config>
void IBuddyAllocator<Config>::deallocate(void *ptr, size_t size) {
  if (size <= static_cast<size_t>(_minSize) && _lazyListSize < _lazyThreshold) {
    BUDDY_DBG("inserting " << ptr - _start << " with index "
                           << block_index(ptr, _numLevels - 1)
                           << " into lazy list");
    push_back(&_lazyList, static_cast<double_link *>(ptr));
    _lazyListSize++;
    BUDDY_DBG("lazy list size: " << _lazyListSize);
    return;
  }

  return deallocate_range(ptr, round_up_pow2(size));
}

// Deallocates a block of memory
template <typename Config> void IBuddyAllocator<Config>::deallocate(void *ptr) {
  if (ptr == nullptr || reinterpret_cast<uintptr_t>(ptr) < _start ||
      reinterpret_cast<uintptr_t>(ptr) >= _start + _totalSize) {
    return;
  }

  size_t size = get_alloc_size(reinterpret_cast<uintptr_t>(ptr));
  BUDDY_DBG("deallocate size: " << size);
  return deallocate(ptr, size);
}

template <typename Config> void IBuddyAllocator<Config>::empty_lazy_list() {
  while (_lazyListSize > 0) {
    auto block = reinterpret_cast<uintptr_t>(pop_first(&_lazyList));
    _lazyListSize--;
    deallocate_single(block);
  }
}

// Prints the free list
template <typename Config> void IBuddyAllocator<Config>::print_free_list() {
  for (size_t i = 0; i < static_cast<size_t>(_numLevels); i++) {
    std::cout << "Free list " << i << "(" << (1U << (_maxBlockSizeLog2 - i))
              << "): ";
    for (double_link *link = _freeList[i].next; link != &_freeList[i];
         link = link->next) {
      std::cout << reinterpret_cast<uintptr_t>(
                       reinterpret_cast<uintptr_t>(link) - _start)
                << " ";
    }
    std::cout << std::endl;
  }
  std::cout << "Lazy list size: " << _lazyListSize << std::endl;
}

// Prints the bitmaps
template <typename Config> void IBuddyAllocator<Config>::print_bitmaps() {
  std::cout << "Allocated blocks: " << std::endl;
  int bitsPerLine = 1;
  int bitsPrinted = 0;
  int spaces = 32;
  for (int i = 0; i < Config::allocedBitmapSize; i++) {
    unsigned int block = _freeBlocks[i];
    for (unsigned int j = 0; j < 8; j++) {
      std::cout << ((block >> j) & 1U);
      for (int k = 0; k < spaces - 1; k++) {
        std::cout << " ";
      }
      bitsPrinted++;
      if (bitsPrinted == bitsPerLine) {
        std::cout << std::endl;
        bitsPerLine *= 2;
        bitsPrinted = 0;
        spaces /= 2;
      }
    }
  }
  std::cout << std::endl;

  std::cout << "Split blocks: " << std::endl;
  if (_sizeMapEnabled) {
    for (int i = 0; i < Config::sizeBitmapSize; i++) {
      unsigned int block = _sizeMap[i];
      for (unsigned int j = 0; j < 8; j++) {
        std::cout << ((block >> j) & 1U) << " ";
      }
    }
    std::cout << std::endl;
  } else {
    for (int i = 0; i < Config::sizeBitmapSize; i++) {
      unsigned int block = _sizeMap[i];
      if (Config::sizeBits == 8) {
        std::cout << size_of_level(block) << " ";
      } else {
        for (unsigned int j = 0; j < 2; j++) {
          std::cout << size_of_level((block >> (j * 4)) & 0xFU) << " ";
        }
      }
    }
    std::cout << std::endl;
  }
}

// Returns the level of the smallest block that can fit the given size
template <typename Config>
int IBuddyAllocator<Config>::find_smallest_block_level(size_t size) {
  for (int i = _minBlockSizeLog2; i <= _maxBlockSizeLog2; i++) {
    if (size <= 1U << static_cast<unsigned int>(i)) {
      BUDDY_DBG("level: " << _maxBlockSizeLog2 - i << " size: " << (1 << i));
      return _maxBlockSizeLog2 - i;
    }
  }
  return -1;
}