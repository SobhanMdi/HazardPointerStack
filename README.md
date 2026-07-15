# Lock-Free Stack with Polymorphic Hazard Pointers

This repository contains a high-performance, thread-safe, and lock-free Singly Linked Stack implemented in modern C++20. 

The primary goal of this implementation is to solve the safe memory reclamation problem (and avoid the ABA problem) without relying on global locks or heavy read-copy-update (RCU) mechanisms. It achieves this by utilizing custom, intrusive Hazard Pointers.

## The Core Problem: Why Hazard Pointers?

In a lock-free environment, multiple threads can concurrently access, push, and pop nodes. If Thread A pops a node and deletes it immediately, Thread B (which was just about to read that node's data) will access deallocated memory, leading to a segmentation fault or memory corruption (Use-After-Free). 

Alternatively, if that memory address is re-allocated quickly, Thread B might incorrectly assume the node state hasn't changed, causing the infamous ABA problem.

To solve this:
1. A reader thread "protects" a node by registering its address in a global registry (Hazard Pointer).
2. A writer/popper thread does not delete the popped node immediately. Instead, it "retires" the node, sending it to a thread-local garbage list.
3. Periodically, when the garbage list reaches a threshold, the thread scans the global hazard registry. Any retired node that is no longer protected by any thread is safely deleted.

## Key Architectural Highlights

### 1. Polymorphic and Intrusive Node Design
Instead of wrapping user data and managing separate deletion callbacks, we designed an intrusive node inheritance scheme:
```text
[Base Class: HazardNode]  <--- Contains virtual destructor & next_retire pointer
^
| (Inherits)
[Derived Class: Node<T>] <--- Contains actual user data T

When the Hazard Pointer subsystem reclaims a node, it only holds a pointer to `HazardNode`. By utilizing the virtual table (VTable) of the polymorphic destructor (`virtual ~HazardNode()`), the compiler automatically resolves the exact derived type `Node<T>` and destroys the underlying data cleanly.

### 2. Cache-Line Alignment (False Sharing Prevention)
In highly concurrent multi-threaded systems, if thread-specific states are stored close to each other in memory, they might fall into the same CPU cache line. When one thread modifies its state, the entire cache line is invalidated for other CPUs, causing massive performance drops (cache bouncing / false sharing).

We enforce strict cache-line alignment on our Hazard Pointer records using:
cpp
struct alignas(std::hardware_destructive_interference_size) HPRecord
This ensures each thread's active record sits on its own cache line, maintaining maximum CPU cache-locality.

### 3. Store-Load Barrier (The Sequential Consistency Fence)
When protecting a pointer, there is a classic race condition where the CPU might reorder the read of the head pointer before the write of the hazard pointer. To guarantee that our hazard pointer reservation is visible to all other threads before we double-check the validity of the pointer, we enforce a strict sequential consistency fence:
cpp
std::atomic_thread_fence(std::memory_order_seq_cst);
This physical hardware barrier prevents CPU instruction pipeline reordering.

## Project Structure

* `main.cpp`: Contains the core implementation of `HazardNode`, `HazardPointerDomain`, `ThreadContext`, `LockFreeStack`, and a heavy stress-test driver simulating concurrent producers and consumers.
* `CMakeLists.txt`: Standard CMake build file targeting C++20 with strict compilation flags.

## Limitations

* **Max Thread Limit:** The current system uses a static domain registry with `MAX_HP = 128`. This means up to 128 active threads can safely acquire hazard pointer slots simultaneously. If your application spawns more than 128 concurrent worker threads accessing this domain, it will throw a runtime exception.

## Building and Running the Stress Test

### Requirements
* A compiler with full C++20 support (GCC 10+, Clang 12+, or MSVC 2019+)
* CMake 3.16+

### Execution
Run the following standard commands:

bash
mkdir build
cd build
cmake ..
cmake --build .
./HazardPointerStack


