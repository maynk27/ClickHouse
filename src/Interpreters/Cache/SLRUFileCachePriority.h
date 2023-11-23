#pragma once

#include <list>
#include <Interpreters/Cache/IFileCachePriority.h>
#include <Interpreters/Cache/FileCacheKey.h>
#include <Interpreters/Cache/LRUFileCachePriority.h>
#include <Common/logger_useful.h>
#include <Interpreters/Cache/Guards.h>

namespace CurrentMetrics
{
    extern const Metric FilesystemCacheSizeLimit;
}

namespace DB
{

/// Based on the SLRU algorithm implementation, the record with the lowest priority is stored at
/// the head of the queue, and the record with the highest priority is stored at the tail.
class SLRUFileCachePriority : public IFileCachePriority
{
private:
    using LRUIterator = LRUFileCachePriority::LRUIterator;
    using LRUQueue = std::list<Entry>;

public:
    class SLRUIterator;

    SLRUFileCachePriority(size_t max_size_, size_t max_elements_, double size_ratio);

    size_t getSize(const CacheGuard::Lock & lock) const override;

    size_t getElementsCount(const CacheGuard::Lock &) const override;

    IteratorPtr add(KeyMetadataPtr key_metadata, size_t offset, size_t size, const CacheGuard::Lock &) override;

    bool collectCandidatesForEviction(
        size_t size,
        FileCacheReserveStat & stat,
        EvictionCandidates & res,
        IFileCachePriority::IteratorPtr reservee,
        FinalizeEvictionFunc & finalize_eviction_func,
        const CacheGuard::Lock &) override;

    void shuffle(const CacheGuard::Lock &) override;

    FileSegments dump(const CacheGuard::Lock &) override;

private:
    LRUFileCachePriority protected_queue;
    LRUFileCachePriority probationary_queue;
    Poco::Logger * log = &Poco::Logger::get("SLRUFileCachePriority");

    void increasePriority(SLRUIterator & iterator, const CacheGuard::Lock & lock);
};

class SLRUFileCachePriority::SLRUIterator : public IFileCachePriority::Iterator
{
    friend class SLRUFileCachePriority;
public:
    SLRUIterator(
        SLRUFileCachePriority * cache_priority_,
        std::unique_ptr<LRUIterator> lru_iterator_,
        bool is_protected_);

    const Entry & getEntry() const override;

    size_t increasePriority(const CacheGuard::Lock &) override;

    void remove(const CacheGuard::Lock &) override;

    void invalidate() override;

    void updateSize(int64_t size) override;

    bool isProtected() const { return is_protected; }

private:
    void assertValid() const;

    SLRUFileCachePriority * cache_priority;
    mutable std::unique_ptr<LRUIterator> lru_iterator;
    bool is_protected;
};

}
