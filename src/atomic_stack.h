
#include <atomic>
#include <cassert>
#include <cstddef>

template <typename T>
class VersionedPointer {
 private:
  T *ptr;
  uint64_t version;
 public:

  /*
   * Default Constructor
   */
  VersionedPointer(T *p_ptr) noexcept :
    ptr{p_ptr},
    version{0UL}
  {}

  /*
   * Default Constructor - This is required by std::atomic
   */
  VersionedPointer() noexcept :
    ptr{nullptr},
    version{0UL}
  {}

  /*
   * Const Pointer Dereference
   */
  const T &operator*() const {
    return *ptr;
  }

  /*
   * Pointer Dereference
   */
  T &operator*() {
    return *ptr;
  }

  /*
   * Member Access Operator
   */
  T *operator->() {
    return ptr;
  }

  /*
   * Prefix operator--
   */
  VersionedPointer &operator--() {
    ptr--;

    return *this;
  }

  /*
   * Postfix operator--
   */
  VersionedPointer operator--(int) {
    VersionedPointer vp{*this};

    ptr--;

    return vp;
  }

  /*
   * Prefix operator++
   */
  VersionedPointer &operator++() {
    ptr++;

    return *this;
  }

  /*
   * Postfix operator++
   */
  VersionedPointer operator++(int) {
    VersionedPointer vp{*this};

    ptr++;

    return vp;
  }

  /*
   * Subtract Operator
   */
  std::ptrdiff_t operator-(const T *target) {
    return ptr - target;
  }

  /*
   * Comparison Operator
   */
  bool operator==(const T *target) {
    return target == ptr;
  }

  /*
   * Smaller Than Operator
   */
  bool operator<(const T *target) {
    return ptr < target;
  }

  /*
   * ToNextVersion() - Go to the next version
   *
   * This should be called before CAS to announce to other threads
   * that the pointer version has changed no matter whether the physical
   * pointer value is equal or not
   */
  void ToNextVersion() {
    version++;

    return;
  }
};

/*
 * class AtomicStack - Thread-safe lock-free stack implementation
 *
 * DO NOT USE THIS CLASS BEFORE YOU HAVE READ THE FOLLOWING:
 *
 * 1. This implementation uses a fixed sized array as stack base.
 *    Please make it sufficiently large in the situation where data
 *    item count could be upper bounded. If not please use a linked-list
 *    based one
 * 2. This implementation does not check for any out of bound access under
 *    release mode (in debug mode it checks). Please be careful and again
 *    make sure the actual size of the stack could be upper bounded
 * 3. Type T is required to be trivially copy assign-able and trivially
 *    constructable. The first is required to copy the element
 *    into the stack, while the second one is used to return an empty value
 *    if the stack is empty
 * 4. Currently the stack implementation only supports many consumer single
 *    producer paradigm, which fits into BwTree's NodeID recycling mechanism
 *    (i.e. one epoch thread freeing deleted NodeID and many worker threads
 *    asking for unused NodeID). In the future it is unlinkly that we would
 *    add MultiThreadPush() or something like that
 */
template <typename T, size_t STACK_SIZE>
class AtomicStack {
 private:
  // This holds actual data
  T data[STACK_SIZE];

  // The pointer for accessing the top of the stack
  // The invariant is that before and after any atomic push()
  // and pop() calls the top pointer always points to the
  // latest valid element in the stack
  //
  // NOTE: We use versioned pointer to avoid ABA problem related with
  // CAS instruction
  std::atomic<VersionedPointer<T>> top_p  __attribute__((aligned (16)));

  // This is used to buffer Push() requests in a single threaded
  // environment
  std::vector<T> buffer;

  /*
   * PreparePush() - Switch the stack top pointer to nullptr
   *
   * This eliminates contention between Push() and Pop(). But it does not
   * eliminate possible contention between multithreaded Push()
   *
   * This function could be called in multi-threaded environment
   *
   * The return value is the stack top before switching it to nullptr
   */
  inline VersionedPointer<T> PreparePush() {
    bool cas_ret;

    // If CAS fails this value would be automatically reloaded
    VersionedPointer<T> snapshot_top_p = top_p.load();

    // Switch the current stack to empty mode by CAS top pointer
    // to nullptr
    // This eliminates contention between Push() and Pop()
    do {
      #ifdef BWTREE_DEBUG
      assert((snapshot_top_p - data + 1) < STACK_SIZE);
      #endif

      // This contends with Pop()
      cas_ret = top_p.compare_exchange_strong(snapshot_top_p,
                                              nullptr);
    } while(cas_ret == false);

    return snapshot_top_p;
  }

 public:

    /*
     * Default Constructor
     *
     * The item pointer is initialized to point to the item before the first item
     * in the stack. This is quite dangerous if the implementation is buggy since
     * it corrupts other data structures.
     */
    AtomicStack() :
     top_p{((T *)data) - 1}
    {}

    /*
     * SingleThreadBufferPush() - Do not directly push the item but keep it
     *                            inside the internal buffer
     *
     * This is used as an optimization for batch push, because the overhead
     * for a single push is rather large (doing 1 CAS and 1 atomic assignment)
     * so we might want to buffer pushes first, and then commit them together
     *
     * This function could only be called in Single Threaded environment
     */
    inline void SingleThreadBufferPush(const T &item) {
      buffer.push_back(item);

      return;
    }

    /*
     * SingleThreadPush() - Push an element into the stack
     *
     * This function calls PreparePush() to switch stack top pointer to nullptr
     * first before writing the new value. This is to avoid advancing the
     * stack pointer without writing the actual value, and then another
     * Pop() thread comes and pops an invalid value.
     *
     * This function must be called in single-threaded environment
     */
    inline void SingleThreadPush(const T &item) {
      VersionedPointer<T> snapshot_top_p = PreparePush();

      // Writing in the pushed value without worrying about concurrent Pop()
      // since all Pop() would fail and return as if the stack were empty
      *(++snapshot_top_p) = item;

      // Before storing into the pointer let it go to the next version first
      snapshot_top_p.ToNextVersion();

      // After this point Pop() would work
      top_p.store(snapshot_top_p);

      return;
    }

    /*
     * SingleThreadCommitPush() - Commit all buffered items
     *
     * This function does a batch commit using only 1 CAS and 1 atomic store
     *
     * This function must be called under multi-threaded environment
     */
    inline void SingleThreadCommitPush() {
      VersionedPointer<T> snapshot_top_p = PreparePush();

      for(auto &item : buffer) {
        *(++snapshot_top_p) = item;
      }

      snapshot_top_p.ToNextVersion();

      top_p.store(snapshot_top_p);
      buffer.clear();

      return;
    }

   /*
    * Pop() - Pops one item from the stack
    *
    * NOTE: The reutrn value is a pair, the first element being a boolean to
    * indicate whether the stack is empty, and the second element is a type T
    * which is the value fetched from the stack if the first element is true
    *
    * NOTE 2: Pop() operation is parallel. If there is a contention with Push()
    * then Pop() will return false positive on detecting whether the stack is
    * empty, even if there are elements in the stack
    *
    * If the return value is pair{false, ... } then value T is default
    * constructed
    */
    inline std::pair<bool, T> Pop() {
      #ifdef BWTREE_DEBUG
      assert((snapshot_top_p - data) < STACK_SIZE);
      #endif
      
      // Load current top pointer and check whether it points to a valid slot
      // If not then just return false. But the item might be modified
      VersionedPointer<T> snapshot_top_p = top_p.load();

      while(1) {
        // This should be true if
        // 1. snapshot_top_p is data - 1 (stack is empty)
        // 2. snapshot_top_p is nullpre (there is Pop() going on)
        if(snapshot_top_p == nullptr) {
          //continue;
          // Avoid spinning here, and just return as an empty stack
          return {false, T{}};
        }

        if(snapshot_top_p < data) {
          // We need to trivially construct a type T element as a place holder
          return {false, T{}};
        }

        // First construct a return value
        auto ret = std::make_pair(true, *snapshot_top_p);

        // Copy construct a new pointer
        VersionedPointer<T> new_snapshot_top_p = snapshot_top_p;
        
        // Move the previous pointer back by 1 element
        new_snapshot_top_p--;
        
        // Update version number to avoid ABA problem
        new_snapshot_top_p.ToNextVersion();

        // Then update the top pointer
        // Avoid ABA problem here by using double word compare and swap
        bool cas_ret = top_p.compare_exchange_strong(snapshot_top_p,
                                                     new_snapshot_top_p);

        // If CAS succeeds which indicates the pointer has not changed
        // since we took its snapshot, then just return
        // Otherwise, need to retry until success or the stack becomes empty
        if(cas_ret == true) {
          return ret;
        }
      }

     assert(false);
     return {false, T{}};
   }
};
