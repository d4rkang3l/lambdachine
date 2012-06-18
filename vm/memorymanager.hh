#ifndef _MEMORYMANAGER_H_
#define _MEMORYMANAGER_H_

#include "common.hh"
#include "utils.hh"
#include "objects.hh"
#include <iostream>

_START_LAMBDACHINE_NAMESPACE

class MemoryManager;

// Only one OS thread should allocate to each block.

class Block
{
public:
  // Start of data in this block.
  inline char *start() const { return start_; }

  // First free byte in the block.
  inline char *free() const { return free_; }

  // Points past last free byte.
  inline char *end() const { return end_; }
  
  // Allocate a number of bytes in this block.  Returns NULL
  // if no room left.
  inline char *alloc(size_t bytes) {
    char *ptr = free_;
    free_ += bytes;
    if (LC_UNLIKELY(free_ > end_)) {
      free_ = ptr;
      return NULL;
    }
    return ptr;
  }

  static const int kBlockSizeLog2 = 15; // 32K
  static const size_t kBlockSize = ((size_t)1) << kBlockSizeLog2;
  static const Word kBlockMask = kBlockSize - 1;

#define DEFINE_CONTENT_TYPE(_) \
  _(Uninitialized,  FREE) \
  _(Closures,       HEAP) \
  _(StaticClosures, STAT) \
  _(InfoTables,     INFO) \
  _(Strings,        STRG) \
  _(Bytecode,       CODE)

  typedef enum {
#define DEF_CONTENT_CONST(name, shortname) k##name,
    DEFINE_CONTENT_TYPE(DEF_CONTENT_CONST)
    kMaxContentType,
#undef DEF_CONTENT_CONST
    kContentsMask = 0xff,
    kScavenged = 0x100,
    kFull = 0x200,
  } Flags;

  inline Flags flags() const {
    return static_cast<Flags>(flags_);
  }

  inline Flags contents() const {
    return static_cast<Flags>(flags_ & kContentsMask);
  }

  friend std::ostream& operator<<(std::ostream&, const Block&);

private:
  friend class Region;
  friend class MemoryManager;
  Block() {}; // Hidden
  ~Block() {};
  void operator delete(void *) {}; // Forbid deleting Blocks

  char *start_;
  char *end_;
  char *free_;
  Block *link_;
  uint32_t flags_;
#if LC_ARCH_BITS == 64
  uint32_t padding;
#endif
};


class Region {
public:
  typedef enum {
    kSmallObjectRegion = 1 // The region is subdivided into blocks.
    //    kLargeObjectRegion,	// The region contains large objects.
  } RegionType;

  static const int kRegionSizeLog2 = 20; /* 1MB */
  static const size_t kRegionSize = 1UL << kRegionSizeLog2;
  static const Word kBlocksPerRegion = kRegionSize / Block::kBlockSize;
  static const Word kRegionMask = kRegionSize - 1;

  // Allocate a new memory region from the OS.
  static Region *newRegion(RegionType);

  static inline char* alignToRegionBoundary(char *ptr) {
    Word w = reinterpret_cast<Word>(ptr);
    return
      reinterpret_cast<char*>(roundUpToPowerOf2(kRegionSizeLog2, w));
  };

  static inline char* alignToBlockBoundary(char *ptr) {
    Word w = reinterpret_cast<Word>(ptr);
    return
      reinterpret_cast<char*>
        (roundUpToPowerOf2(Block::kBlockSizeLog2, w));
  };

  static inline Region *regionFromPointer(void *p) {
    return reinterpret_cast<Region*>((Word)p & ~kRegionMask);
  }

  static inline Block *blockFromPointer(void *p) {
    Region *r = regionFromPointer(p);
    Word index = ((Word)p & kRegionMask) >> Block::kBlockSizeLog2;
    LC_ASSERT(0 <= index && index < kBlocksPerRegion);
    return &r->blocks_[index];
  }

  // Unlink and return a free block from the region.
  //
  // Returns NULL if this region has no more free blocks.
  Block *grabFreeBlock();

  static void operator delete(void *p);
  ~Region();

  friend std::ostream& operator<<(std::ostream& out, const Region&);
  friend std::ostream& operator<<(std::ostream& out, const MemoryManager&);

  inline const char *regionId() const { return (const char*)this; }

private:
  Region() {}  // Hidden
  void initBlocks();

  Word region_info_;
  Block blocks_[kBlocksPerRegion];
  Region *region_link_;
  Block *next_free_;

  friend class MemoryManager;
};

class MemoryManager
{
  //  void *allocInfoTable(Word nwords);
public:
  MemoryManager();
  ~MemoryManager();

  inline InfoTable *allocInfoTable(Word nwords) {
    return static_cast<InfoTable*>
      (allocInto(&info_tables_, nwords * sizeof(Word)));
  }

  inline char *allocString(size_t length) {
    return reinterpret_cast<char*>(allocInto(&strings_, length + 1));
  }

  inline Closure *allocStaticClosure(size_t payloadSize) {
    return static_cast<Closure*>
      (allocInto(&static_closures_,
                 (wordsof(ClosureHeader) + payloadSize) * sizeof(Word)));
  }

  inline void *allocCode(size_t instrs, size_t bitmaps) {
    return allocInto(&bytecode_,
                     sizeof(BcIns) * instrs + sizeof(u2) * bitmaps);
  }

  inline Closure *allocClosure(InfoTable *info, size_t payloadWords) {
    Closure *cl = reinterpret_cast<Closure*>
      (allocInto(&closures_, wordsof(ClosureHeader) + payloadWords * sizeof(Word)));
    Closure::initHeader(cl, info);
    return cl;
  }

  bool looksLikeInfoTable(void *p);
  bool looksLikeClosure(void *p);

  unsigned int infoTables();

  friend std::ostream& operator<<(std::ostream& out, const MemoryManager&);

  inline uint64_t allocated() const { return allocated_; }

private:
  inline void *allocInto(Block **block, size_t bytes) {
    char *ptr = (*block)->alloc(bytes);
    while (LC_UNLIKELY(ptr == NULL)) {
      blockFull(block);
      ptr = (*block)->alloc(bytes);
    }
    allocated_ += bytes;
    return ptr;
  }

  inline bool isGCd(Block *block) const {
    return block->contents() == Block::kClosures;
  }

  friend class Capability;

  // There must not be any other allocation occurring to this block.
  inline void getBumpAllocatorBounds(char **heap, char **heaplim) {
    *heap = closures_->free();
    *heaplim = closures_->end();
    LC_ASSERT(isWordAligned(*heap));
    LC_ASSERT(isWordAligned(*heaplim));
  }

  inline void sync(char *heap, char *heaplim) {
    // heaplim == NULL can happen if we want to force a thread to
    // yield.
    LC_ASSERT(heaplim == NULL || heaplim == closures_->end());
    LC_ASSERT(closures_->free() <= heap && heap < closures_->end());
    allocated_ += static_cast<uint64_t>(heap - closures_->free());
    closures_->free_ = heap;
  }

  void bumpAllocatorFull(char **heap, char **heaplim);

  Block *grabFreeBlock(Block::Flags);
  void blockFull(Block **);

  Region *region_;
  Block *full_;
  Block *free_;
  Block *info_tables_;
  Block *static_closures_;
  Block *closures_;
  Block *strings_;
  Block *bytecode_;

  // Assuming an allocation rate of 16GB/s (pretty high), this counter
  // will overflow in 2^30 seconds, or about 34 years.  That appears
  // to be fine for now (it's for statistical purposes only).
  uint64_t allocated_;
};

_END_LAMBDACHINE_NAMESPACE

#endif /* _MEMORYMANAGER_H_ */