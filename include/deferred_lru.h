#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

struct EmptyDeletePolicy {
    template <typename KeyT, typename ValueT>
    void onDelete(const KeyT&, const ValueT&) {}
};

/**
 * # DeferredLRU
 * DeferredLRU is a concurrent LRU key-value cache.
 *
 * It is based on a conjunction of hash table
 * (for fast item lookup) and intrusive doubly
 * linked list (for tracking least recently items).
 *
 * ## Glossary
 * Recent list
 *   ...
 * Recent head
 *   ...
 * Recent node
 *   Node that is in the recent list
 * Purge
 *   ...
 * Node
 *   ...
 * LRU list
 *   ...
 * LRU head
 *   ...
 * LRU tail
 *   ...
 * Empty list
 *   ...
 * Item
 *   Same as node
 * Hash table
 *   ...
 * Bucket
 *   ...
 * Capacity constraints
 *   ...
 * Pull/purge token
 *   ...
 * Node pull
 *   ...
 *
 *
 * ## Supported operations
 *
 *   - Basic operations
 *      - FIND node by key and mark this node as recently accessed
 *      - INSERT new node (this can trigger PURGE
 *                         if the cache is full)
 *      - PULL RECENT nodes (marked as recent by FIND operation)
 *        into LRU list head
 *      - PURGE OLD recently nodes from cache to free some memory
 *
 *   - Additional internal operations
 *     - MARK RECENT - atomically add a node to the current recent list
 *       if has not been added yet
 *     - ADD TO LRU - atomically insert node into LRU head
 *                    FIXME: head->next should not be touched by PURGE/PULL
 *                    FIXME: head is concurrently changed (locking?)
 *     - UPDATE CAPACITY - increase node count and dynamic size memory count
 *     - GET RECENT SLICE - get head of the current recent list.
 *                          Can be done only by a thread with recent list token.
 *     - EVICT FROM LRU - remove node from a LRU list
 *                        FIXME: don't remove node that is referred by head
 *     - BULK ADD TO LRU - atomically insert a sublist into LRU head
 *     - TRY REMOVE FROM BUCKET
 *     - ADD TO BUCKET
 *     - GET NODE FROM POOL
 *     - DUMP NODE TO POOL
 *
 *
 * ### Find
 *   > h := hash(key)
 *   > bucked := ht[h % ht_size]
 *   > lock bucket
 *   >   found := traverse items
 *   >   if found:
 *   >     consume node->value
 *   >     MARK RECENT node
 *   > unlock bucket
 *   >
 *   > if required:
 *   >   PULL RECENT
 *   >
 *   > return found
 *
 *
 * ### Insert
 *   Node can't be evicted before it is added ot a bucket:
 *      First we add node to hash table and only then to a LRU list.
 *      This way we prevent an (unlike, but still possible) scenario,
 *      when inserted node is attempted to be purged before
 *      it is inserted into a bucket. (TODO: detailed explanation).
 *
 *   Node can't be evicted before it is added to LRU list:
 *      Artificial RECENT link (TODO: explain)
 *
 *   > node := GET EMPTY NODE
 *   > node.key/value = key/value
 *   > node.recent_link = AUX_RECENT
 *   >
 *   > UPDATE CAPACITY with node
 *   >
 *   > h := hash(key)
 *   > bucked := ht[h % ht_size]
 *   > lock bucket
 *   >   add node to bucket
 *   >   unlock bucket
 *   > unlock bucket
 *   >
 *   > ADD node TO LRU
 *   > node.recent_link = NULL
 *   > FIXME: atomically set node.recent_link while inserting to head
 *   > if capacity constraints are violated:
 *   >   PURGE OLD
 *
 * ### PULL RECENT
 *   > try obtain pull/purge token:
 *   > if failed:
 *   >   exit (other thread is already handling this)
 *   >
 *   > recent_head := GET RECENT SLICE
 *   > lru_temp_head := NONE
 *   >
 *   > for each node in head:
 *   >   evict node from LRU
 *   >   FIXME: LRU head refers to node
 *   >   append node to a lru_temp_head
 *   >
 *   > BULK ADD lru_temp_head TO LRU
 *   > FIXME: how to atomically reset all recent_link? (refer to flag)
 *   >
 *   > revoke pull/purge token
 *
 * ### PURGE OLD
 *   > try obtain pull/purge token:
 *   > if failed:
 *   >   exit (other thread is already handling this)
 *   >
 *   > traverse backward from LRU tail
 *   >   TODO: handle RECENT nodes with stub links
 *   >   if node is RECENT:
 *   >     try REMOVE FROM BUCKET
 *   >       if success:
 *   >         delete content
 *   >         ADD node TO POOL
 *   >   # else we just skip it, it will be handled
 *   >   # by the followed PULL RECENT operation
 *   > PULL RECENT
 *   > TODO: try to take whole tail as a sublist (tag pointer?)
 *
 *
 * - dynamic memory
 * - recent list
 * - double list head
 * - pull tail sublist as whole
 *
 * - concurrency
 *  - synchronization
 *  - atomic operations
 *  - deadlock prevention
 *  - invariants
 *  - value retrieval in concurrent environment
 * - parameter search (purge limit/pull limit)
 *
 * ## Notes
 *
 * - Comparison with Bag-LRU
 * - Do not access head neighbor
 * - Fake recent list tail
 * - Memory barrier on when adding to recent list
 */
template <typename KeyT, typename ValueT,
        typename DeletionPolicyT = EmptyDeletePolicy,
        typename LockT = std::mutex,
        typename HasherT = std::hash<KeyT>,
        int HashtableLoadFactor = 4>
class DeferredLRU {
  public:
    using key_t             = KeyT;
    using value_t           = ValueT;
    using deletion_policy_t = DeletionPolicyT;
    using lock_t            = LockT;
    using hasher_t          = HasherT;

    template <typename T>
    using atomic_t = std::atomic<T>;

  private:
    /**
     * Defined with idea, that for T thread probability of 2 threads
     * accessing the same lock should * be <= p.
     * Probability is calculated as in the Birthday paradox.
     * With T == 48, p ~= 0.03, size = 2^15
     */
    static constexpr size_t maxBucketLockSize() { return 1 << 15; }

    static constexpr size_t bucketLockIndexMask() {
        return maxBucketLockSize() - 1; // gives 000111 mask for MAX=001000
    }

    constexpr static int hashtableLoadFactor() { return HashtableLoadFactor; }

    struct NodeBase {
        atomic_t<NodeBase*> lru_next = {nullptr};
        atomic_t<NodeBase*> lru_prev = {nullptr};
        atomic_t<NodeBase*> recent_next = {nullptr};
        NodeBase* bucket_next = {nullptr};
    };

    struct Node : NodeBase {
        key_t key;
        value_t value;
    };

    struct BucketHead {
        Node* bucket_next = nullptr;
    };

  public:
    explicit DeferredLRU(size_t capacity = 0, bool is_item_capacity = false,
                         double pull_threshold_factor = 0.1, double purge_threshold_factor = 0.1) {
        /// initialize a cache that stores size objects.
        /// Subsequently added object will cause an eviction.
        allocateMemory(capacity, is_item_capacity, pull_threshold_factor, purge_threshold_factor);
    }

    ~DeferredLRU() { releaseMemory(); }

    static double elementSize() {
        return sizeof(Node) + sizeof(BucketHead) / (double)hashtableLoadFactor();
    }

    void allocateMemory(size_t capacity, bool is_item_capacity, double pull_threshold_factor = 0.1,
                        double purge_threshold_factor = 0.1) {}

    /// calls the eviction policy on all the objects in the cache
    void releaseMemory() {
        for (BucketHead& bucket : buckets_) {
            Node* node = bucket.bucket_next;
            while (node) {
                deleter_.onDelete(std::move(node->key), std::move(node->value));
                node = (Node*)(node->bucket_next);
            }
        }

        nodes_.reset();
        buckets_.clear();
        buckets_.shrink_to_fit();
        bucket_locks_.reset();
    }

    size_t capacity() const { return max_element_count_; }

    size_t approximateSize() const { return current_element_count_; }

    /**
     * Lock a bucket that is associated with the key,
     * find a node with the same key in the bucket.
     * If found, write it to consumer and mark the node as recent,
     * conditionally requesting pull op and doing a consolidation.
     *
     * @tparam ValueConsumer
     * @param key
     * @param consumer
     * @return true if the key was found
     */
    template <typename ValueConsumer>
    bool find(const key_t& key, ValueConsumer& consumer) {
        auto bucket_nr = keyToBucketNr(key);
        lockBucket(bucket_nr);

        Node* node = searchBucket(key, bucket_nr);
        bool found = node != nullptr;

        if (found) {
            consumer = node->value;
            markNodeRecent(node);
        }

        unlockBucket(bucket_nr);

        if (recentThresholdHit()) {
            requestPull();
        }

        return found;
    }

    /**
     * Acquire an empty node. If there is no such node or some
     * other capacity constraints (e.g. dynamic memory is exceeded)
     * purge op may be triggered or the caller may spin
     * if other thread is currently performing it.
     *
     * Initialize the acquired node with the passed key and value,
     * insert it to both hash table and LRU list.
     *
     * Insert to a hash table first. Otherwise, the node could be
     * evicted by the purge op before it was inserted into a bucket.
     * Purging thread would deadlock when trying to remove
     * a nonexistent node from a bucket.
     *
     * While node is being inserted into LRU list, it may be found in a bucket
     * by another thread, that will attempt to add it to Recent list.
     * In pull op is started at the same time, pulling thread would try
     * to remove the node from LRU list, while it's not there.
     *
     * In order to prevent this, a dummy pointer is written to node
     * recent link while the node is in * the hash table,
     * but not in the LRU list. This falsely marks node as in
     * Recent list preventing other threads to add it to the actual one.
     * As soon as the node is added to the LRU list, the recent link is reset
     * to nullptr, allowing the node to be added into the Recent list next time.
     *
     * @tparam ForwardKeyT
     * @tparam ForwardValueT
     * @param key
     * @param value
     */
    template <typename ForwardKeyT, typename ForwardValueT>
    void insert(ForwardKeyT&& key, ForwardValueT&& value) {
        // reserving space for the element beforehand
        this->current_element_count_++;

        // get new node from pool
        // if pool is empty, we may trigger purge op to find some
        // or SPIN if other thread is currently doing it
        Node* node = allocateNode();
        node->key = std::forward<ForwardKeyT>(key);
        node->value = std::forward<ForwardValueT>(value);

        // prevent node from being marked as recent since it's not in LRU yet
        node->recent_next.store(recentDummyTerminalPtr(), std::memory_order_seq_cst);

        addNodeToBucket(node);
        addNodeToLruHead(node);

        // node now can participate in recent list
        node->recent_next.store(nullptr, std::memory_order_release);
    }

    /**
     * First, lookup the required key in the cache.
     * If found, copy the corresponding value to the consumer.
     *
     * Otherwise, recalculate the value with the producer callback.
     * Insert the new key-value pair into the cache and copy it to the consumer.
     *
     * @tparam Producer
     * @tparam Consumer
     * @param key
     * @param producer
     * @param consumer
     */
    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        if (find(key, consumer)) {
            return;
        }

        auto x = producer();
        consumer = x;
        insert(key, std::move(x));
    }

  private:
    static size_t getBucketCountForCapacity(size_t capacity) {
        return (size_t)(capacity + hashtableLoadFactor() - 1) / hashtableLoadFactor();
    }

    bool requestPull() {
        pull_request_ = true;
        return consolidateCache();
    }

    bool requestPurge() {
        purge_request_ = true;
        return consolidateCache();
    }

    bool consolidateCache() {
        if (lru_lock_.try_lock()) {
            if (purge_request_) {
                purgeOld(purge_threshold_);
                purge_request_ = false;
            }

            if (pull_request_) {
                pullRecent();
                pull_request_ = false;
            }

            lru_lock_.unlock();
            return true;
        } else {
            return false;
        }
    }

    void pullRecent() {
        NodeBase* current = popRecentListSlice();

        NodeBase head;
        NodeBase* prev = &head;

        while (current != recentDummyTerminalPtr()) {
            // TODO memory order?
            if (current->lru_prev == &lru_head_) {
                // skip this node
                NodeBase* next = current->recent_next.load(std::memory_order_relaxed);
                current->recent_next.store(nullptr, std::memory_order_relaxed);
                current = next;
            } else {
                // extract node from LRU
                removeNodeFromLru(current);

                // add it to temp list
                prev->lru_next.store(current, std::memory_order_relaxed);
                current->lru_prev.store(prev, std::memory_order_relaxed);

                prev = current;
                current = prev->recent_next.load(std::memory_order_relaxed);
                prev->recent_next.store(nullptr, std::memory_order_relaxed);
            }
        }

        if (prev == &head) {
            // No recent nodes found
            return;
        }

        addSublistToLruHead(head.lru_next.load(std::memory_order_relaxed), prev);
    }

    void purgeOld(size_t required_nodes) {
        size_t nodes_freed = 0;
        size_t recent_seen = 0;

        if (required_nodes == 0) {
            required_nodes = 1;
        }

        NodeBase* node = lru_tail_.lru_prev.load(std::memory_order_relaxed);

        // TODO remove check
        while (node != &lru_head_ && nodes_freed < required_nodes) {
            NodeBase* next = node->lru_prev.load(std::memory_order_acquire);
            if (next == &lru_head_) {
                break;
            }

            if (!markedRecent(node)) {
                Node* typed_node = reinterpret_cast<Node*>(node);
                // Can fail if node was JUST marked recent
                if (removeNodeFromBucket(typed_node, false)) {
                    removeNodeFromLru(typed_node);

                    deleter_.onDelete(std::move(typed_node->key), std::move(typed_node->value));
                    this->current_element_count_--;
                    disposeNode(node);

                    nodes_freed++;
                } else {
                    recent_seen++;
                }
            } else {
                recent_seen++;
            }

            node = next;
        }
    }

    void addNodeToLruHead(NodeBase* node) { addSublistToLruHead(node, node); }

    void addSublistToLruHead(NodeBase* first, NodeBase* last) {
        first->lru_prev.store(&lru_head_, std::memory_order_relaxed);
        NodeBase* current_next = lru_head_.lru_next.load(std::memory_order_relaxed);

        do {
            last->lru_next.store(current_next, std::memory_order_relaxed);
        } while (!lru_head_.lru_next.compare_exchange_weak(current_next, first));

        current_next->lru_prev.store(last, std::memory_order_release);
    }

    void removeNodeFromLru(NodeBase* node) {
        auto prev = node->lru_prev.load(std::memory_order_relaxed);
        auto next = node->lru_next.load(std::memory_order_relaxed);
        prev->lru_next.store(next, std::memory_order_relaxed);
        next->lru_prev.store(prev, std::memory_order_relaxed);
    }

    /**
     *
     * @param node expected to be locked
     */
    void markNodeRecent(NodeBase* node) {
        if (!markedRecent(node)) {
            // memory fence after recent check
            NodeBase* next = recent_head_.load(std::memory_order_acquire);
            do {
                node->recent_next.store(next, std::memory_order_relaxed);
            } while (!recent_head_.compare_exchange_weak(next, node));

            recent_count_++;
        }
    }

    bool markedRecent(NodeBase* node) {
        return node->recent_next.load(std::memory_order_relaxed) != nullptr;
    }

    bool recentThresholdHit() {
        return recent_count_.load(std::memory_order_relaxed) >= pull_threshold_;
    }

    NodeBase* popRecentListSlice() {
        NodeBase* slice = recent_head_.exchange(recentDummyTerminalPtr());
        recent_count_.store(0, std::memory_order_relaxed);
        // Possible race condition is harmless
        return slice;
    }

    Node* allocateNode() {
        NodeBase* node = empty_head_.load(std::memory_order_acquire);

        // CAS loop to pop first node in empty_head unless empty_head is empty
        while (true) {
            if (node == nullptr) {
                requestPurge();
                // TODO backoff
                // TODO Choose memory model for the following load
                node = empty_head_.load(std::memory_order_seq_cst);
                continue;
            } else {
                NodeBase* next = node->lru_next.load(std::memory_order_relaxed);
                if (empty_head_.compare_exchange_weak(node, next)) {
                    break;
                }
            }
        };

        return (Node*)node;
    }

    void disposeNode(NodeBase* node) {
        NodeBase* next = empty_head_.load(std::memory_order_relaxed);
        do {
            node->lru_next.store(next, std::memory_order_relaxed);
        } while (!empty_head_.compare_exchange_weak(next, node));
    }

    void addNodeToBucket(Node* node) {
        auto bucket_nr = keyToBucketNr(node->key);

        lockBucket(bucket_nr);
        BucketHead& head = buckets_[bucket_nr];
        node->bucket_next = head.bucket_next;
        head.bucket_next = node;
        unlockBucket(bucket_nr);
    }

    Node* searchBucket(const key_t& key, size_t bucket_nr) {
        BucketHead& head = buckets_[bucket_nr];

        Node* node = head.bucket_next;

        while (node) {
            if (node->key == key) {
                return node;
            }
            node = (Node*)(node->bucket_next);
        }

        return nullptr;
    }

    /**
     * Can fail if node doesn't exist in bucket or it is marked as recent
     * @param node
     * @return
     */
    bool removeNodeFromBucket(Node* node, bool remove_if_recent) {
        auto bucket_nr = keyToBucketNr(node->key);
        lockBucket(bucket_nr);
        BucketHead& head = buckets_[bucket_nr];

        if (!remove_if_recent && markedRecent(node)) {
            unlockBucket(bucket_nr);
            return false;
        }

        if (head.bucket_next == node) {
            head.bucket_next = (Node*)(node->bucket_next);
            node->bucket_next = nullptr;
            unlockBucket(bucket_nr);
            return true;
        }

        Node* parent = head.bucket_next;

        while (parent) {
            if (parent->bucket_next == node) {
                parent->bucket_next = node->bucket_next;
                node->bucket_next = nullptr;
                unlockBucket(bucket_nr);
                return true;
            }
            parent = (Node*)(parent->bucket_next);
        }

        unlockBucket(bucket_nr);
        return false;
    }

    NodeBase* recentDummyTerminalPtr() {
        // Arbitrary unique value
        static int dummy = 0;
        return reinterpret_cast<NodeBase*>(&dummy);
    }

    size_t keyToBucketNr(const key_t& key) {
        // TODO: mask operation
        return hasher_(key) % buckets_.size();
    }

    void lockBucket(size_t bucket_nr) { bucket_locks_[bucket_nr & bucketLockIndexMask()].lock(); }

    void unlockBucket(size_t bucket_nr) {
        bucket_locks_[bucket_nr & bucketLockIndexMask()].unlock();
    }

    static size_t maxElementCountForCapacity(size_t capacity) {
        auto s = elementSize();
        return static_cast<size_t>(capacity / s);
    }

  private:
    size_t max_element_count_;
    size_t pull_threshold_;
    size_t purge_threshold_;

    atomic_t<size_t> current_element_count_;

    NodeBase lru_head_;
    NodeBase lru_tail_;

    atomic_t<NodeBase*> empty_head_;

    atomic_t<NodeBase*> recent_head_;
    atomic_t<size_t> recent_count_;

    atomic_t<bool> pull_request_;
    atomic_t<bool> purge_request_;
    lock_t lru_lock_;

    std::unique_ptr<Node[]> nodes_;
    std::vector<BucketHead> buckets_;
    std::unique_ptr<lock_t[]> bucket_locks_;

    hasher_t hasher_;
    deletion_policy_t deleter_;
};
