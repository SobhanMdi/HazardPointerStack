#include <iostream>
#include <utility>
#include <cstdint>
#include <atomic>
#include <thread>
#include <array>
#include <expected>
#include <vector>
#include <algorithm>
#include <optional>
#include <chrono>
#include <new>

// Base garbage structure.
// In this update, instead of storing data inside a RetireNode and deleting it later,
// long story short: with this new structure, we send the node itself directly to the trash bin by its address.
struct HazardNode
{
	// Properly connects the chains of the retire/trash list together.
	HazardNode* next_retire{ nullptr };

	// Polymorphism down here.
	// Before deleting this structure, it checks the virtual table,
	// figures out the real type that inherited from this structure,
	// and deletes that real object first.
	virtual ~HazardNode() = default;
};

// Hazard Pointer Domain class.
// Its job is to store orphan garbage created by threads that are already dead.
// Meaning: a thread was popping, put some nodes inside its local trash bin,
// but then there was no more data to pop and the thread died.
// In that case, its trash bin should not just disappear and leak memory.
// Those garbage nodes become orphaned.
// We need to fairly distribute/manage them between the remaining threads.
// This class is shared between all threads, so it must be lock-free and thread-safe.
class HazardPointerDomain
{
private:
	// Atomic linked list for storing orphan garbage.
	// It is atomic because multiple threads may push their garbage here at the same time.
	std::atomic<HazardNode* > global_retire_list_{ nullptr };

public:
	// Total number of hazard pointers available in the whole system.
	static constexpr size_t MAX_HP = 128;

	// 64-byte aligned structure for cache-line management.
	// Why 64? Because when memory is transferred from RAM to L1 cache,
	// modern CPUs usually move memory in 64-byte cache-line blocks.
	// If structures sit right next to each other and one thread reads from slot one
	// while another thread reads from slot two, this cache line keeps getting invalidated,
	// and that slows the program down.
	// So by sacrificing a tiny bit of memory, we get better speed.
	// -----------------------------Update-------------------------
	// I learned something new:
	// instead of hardcoding 64, which is mainly for modern CPUs,
	// hardware_destructive_interference_size can detect the system's cache-line size at compile time.
	// So the padding becomes suitable and optimized for the target system,
	// and this also makes the code much more flexible and optimized for ARM too.
	struct alignas(std::hardware_destructive_interference_size) HPRecord
	{
		// An atomic flag to see whether this slot has been claimed or not.
		std::atomic<bool> active{ false };

		// Atomic pointer that stores the address of the node currently being read.
		// When it is null, it means nobody is using/protecting any node here.
		std::atomic<void*> ptr{ nullptr };
	};

	// This is our global array.
	// Threads write here when they want to use a node:
	// "Hey, I am working with this node. Nobody is allowed to delete it."
	std::array<HPRecord, MAX_HP> hazard_pointers_;

	// Constructor.
	HazardPointerDomain() = default;

	// Destructor.
	// When all threads are gone, our assumption is that no thread is left anymore.
	// Then we can calmly clean up all remaining garbage.
	~HazardPointerDomain()
	{
		// First we check and read the nodes currently inside the global trash bin.
		// Relaxed is fine because at this point we are not dealing with other threads.
		HazardNode* current = global_retire_list_.load(std::memory_order_relaxed);

		// Keep going while there is still data/node left.
		while (current)
		{
			// Save the next node.
			HazardNode* next = current->next_retire;

			// Polymorphism will use the VTable and find the real type of the data/object,
			// then it deletes it correctly.
			// Now we delete the node itself.
			delete current;

			// Move to the next node and continue the loop until nothing remains.
			current = next;
		}
	}

	// Whenever a thread is born, it claims one slot from the hazard pointer table using a flag.
	// This function is for the time when a new thread wants to get its own hazard pointer slot.
	HPRecord* try_acquire_hp()
	{
		for (size_t i = 0; i < hazard_pointers_.size(); ++i)
		{
			// We assume the slot is empty, so we expect the flag to be false.
			bool expected{ false };

			// Try to claim this slot.
			// If successful, it returns true.
			// We use strong because we already have a for-loop here.
			if (hazard_pointers_[i].active.compare_exchange_strong(expected,
				true,
				std::memory_order_acquire,
				std::memory_order_relaxed))
			{
				// We successfully claimed it, so return its address.
				return &hazard_pointers_[i];
			}

			// Continue until there are no slots left.
			continue;
		}

		// The loop finished and we found no available slot.
		throw std::runtime_error("No available hazard pointers");
	}

	// When a thread is done working with a node and no longer needs it,
	// it must clear its announcement board slot.
	void release_hp(HPRecord* record)
	{
		// First we null the data pointer.
		// Relaxed is enough because the real synchronization message happens next.
		record->ptr.store(nullptr, std::memory_order_relaxed);

		// Now with release, we announce that all changes are visible to other threads,
		// including the null store above.
		record->active.store(false, std::memory_order_release);
	}

	// This method takes a batch of local garbage from other threads
	// and adds them to the global domain list.
	void push_orphaned_nodes(HazardNode* head, HazardNode* tail)
	{
		// First we read and store the current value of the global list.
		HazardNode* old_head = global_retire_list_.load(std::memory_order_relaxed);

		// Now we connect the tail of the local list to the beginning of the global list.
		tail->next_retire = old_head;

		// Death loop.
		// Now the local list is connected to the beginning of the global list,
		// but the global head itself has not been updated yet.
		// We update it with CAS, bulletproof style.
		while (!global_retire_list_.compare_exchange_weak(old_head,
			head,
			std::memory_order_release,
			std::memory_order_relaxed))
		{
			// We failed.
			// Some other thread changed the old global head before us.
			// CAS automatically updates old_head and makes it equal to the real current global head.
			// So we must reconnect the tail of our local list to the new global head
			// and try again.
			tail->next_retire = old_head;
		}
	}
};

// Thread-local context.
// Job: isolate each thread and prevent cache lines from constantly becoming invalidated
// and going back to RAM.
class ThreadContext
{
private:
	// We create a direct tunnel to the global/domain list.
	HazardPointerDomain& domain_;

	// Each thread has its own local list with a head, a tail, and a counter
	// that counts from 0 up to 64.
	HazardNode* head_{ nullptr };
	HazardNode* tail_{ nullptr };
	uint32_t local_count_{ 0 };

	// It also has a threshold.
	// When the local garbage count reaches this limit, we start scanning and deleting nodes.
	static constexpr uint32_t RETIRE_THRESHOLD = 64;

	// When a thread is born, it gets one hazard pointer slot.
	// When it dies, it gives it back. RAII.
	HazardPointerDomain::HPRecord* hp_record_{ nullptr };

public:
	// Constructor receives the global domain and stores it.
	ThreadContext(HazardPointerDomain& d) : domain_(d)
	{
		// The thread is born and claims a slot.
		hp_record_ = domain_.try_acquire_hp();
	}

	// Rule of 5.
	// Thread-local variables belong only to that thread,
	// so copying or moving must not happen.
	ThreadContext(const ThreadContext& other) = delete;
	ThreadContext& operator=(const ThreadContext& other) = delete;
	ThreadContext(ThreadContext&& other) noexcept = delete;
	ThreadContext& operator=(ThreadContext&& other) noexcept = delete;

	// Destructor.
	// Runs when the thread is done or dying.
	~ThreadContext()
	{
		// First we release this thread's hazard pointer slot.
		if (hp_record_)
		{
			domain_.release_hp(hp_record_);
		}

		// First we must check and make sure this thread actually has nodes.
		if (head_ != nullptr)
		{
			// Now we send our local list head and tail to the global list,
			// and the global domain will manage these orphan nodes.
			domain_.push_orphaned_nodes(head_, tail_);
		}
	}

	// When a thread wants to pop a node, it must not delete it immediately,
	// because of ABA and use-after-free problems.
	void retire(HazardNode* node)
	{
		// The new retired node should point to the current head of the local retire list.
		auto NewRetireNode = node->next_retire = head_;

		// Now update the head of the list to the new node.
		head_ = NewRetireNode;

		// If this is the first garbage node we are putting into the bin,
		// then this node is both head and tail.
		// This is important so later the destructor does not get into trouble.
		if (tail_ == nullptr)
		{
			tail_ = NewRetireNode;
		}

		// Now increase the local counter by one.
		++local_count_;

		// If the local counter reached the threshold, start scanning and deleting.
		if (local_count_ >= RETIRE_THRESHOLD)
		{
			scan();
		}
	}

	// This function is used when a thread wants to write the address of the node
	// it is working with into the announcement board.
	// It makes sure no other thread has deleted that node in the middle of the operation,
	// and only then announces/protects it.
	template <typename T>
	T* protect(const std::atomic<T*>& atomic_node_ptr)
	{
		while (true)
		{
			// Read the current pointer value and store it.
			T* current = atomic_node_ptr.load(std::memory_order_relaxed);

			// If it is null, then there is nothing to read, so we return.
			if (current == nullptr)
			{
				return nullptr;
			}

			// We made sure there is data,
			// so we register it on the announcement board.
			// With memory_order_release, we also notify other threads about it.
			hp_record_->ptr.store(current, std::memory_order_release);

			// Warning:
			// Because after the store we quickly do a load,
			// software/hardware may try to move loads before the store.
			// With the line below, we put a very strong wall here
			// and do not let the system reorder things in a dangerous way.
			std::atomic_thread_fence(std::memory_order_seq_cst);

			// Warning:
			// To make sure another thread did not change this pointer while we were working,
			// we check it one more time.
			if (current == atomic_node_ptr.load(std::memory_order_acquire))
			{
				// It was equal, so we successfully protected the current value.
				// Return it and exit the function.
				return current;
			}

			// We failed.
			// Another thread may have deleted or popped that node,
			// or the current value changed.
			// Before trying again, we must clear our record from the announcement board.
			hp_record_->ptr.store(nullptr, std::memory_order_relaxed);
			continue;
		}
	}

	// My global scan method.
	void scan()
	{
		// Store and collect active announcement board entries.
		// Vector is used to store active slots.
		std::vector<void*> activeHazzard;

		// Reserve memory for better performance.
		activeHazzard.reserve(domain_.MAX_HP);

		// Find active slots and store their pointers.
		for (size_t i = 0; i < domain_.hazard_pointers_.size(); ++i)
		{
			// If the flag is on, it means someone has claimed this slot.
			if (domain_.hazard_pointers_[i].active.load(std::memory_order_acquire))
			{
				// Read the pointer once with acquire and store it inside a local variable.
				void* p = domain_.hazard_pointers_[i].ptr.load(std::memory_order_acquire);

				// Now that this slot is occupied,
				// is it actually looking at a specific node?
				if (p != nullptr)
				{
					activeHazzard.emplace_back(p);
				}
			}
		}

		// Sort the vector to improve performance.
		// Searching in a sorted array is faster: O(logN).
		std::sort(activeHazzard.begin(), activeHazzard.end());

		// Create a new local head and tail for keeping garbage nodes
		// that are still being used.
		HazardNode* new_head{ nullptr };
		HazardNode* new_tail{ nullptr };

		// Local counter is set to zero for now,
		// because we are building a new list.
		local_count_ = 0;

		// For this thread, start reading nodes from the head of its garbage list.
		HazardNode* current = head_;

		// Continue while this thread still has nodes inside its trash bin.
		while (current)
		{
			// Save the next node.
			auto next_node = current->next_retire;

			// Question:
			// Does the address of this garbage node exist in the hazard pointer announcement board?
			bool is_hazardous = std::binary_search(activeHazzard.begin(), activeHazzard.end(), current);

			// If it exists, another thread is still using it, so we must not delete it.
			if (is_hazardous)
			{
				// Set this node's next pointer to the head of the new list.
				current->next_retire = new_head;

				// Connect it to our new local garbage list.
				new_head = current;

				// If this is the first node being added to the new list,
				// then the tail is also this same node.
				if (new_tail == nullptr)
				{
					new_tail = current;
				}

				// Increase the local counter.
				++local_count_;
			}
			else
			{
				// No other node/thread is using it anymore, so we can delete it.
				// First we should delete the data inside it using its "will".
				// Update: now it is deleted through polymorphism.
				// current->deleter(current->ptr);

				// Now delete the node itself.
				delete current;
			}

			// Continue the loop with the next node.
			current = next_node;
		}

		// Now update this thread's trash-bin list with the new list.
		tail_ = new_tail;
		head_ = new_head;
	}

	// This method clears the thread's announcement board slot
	// when the thread is done working with the node.
	void clear_hp()
	{
		hp_record_->ptr.store(nullptr, std::memory_order_relaxed);
	}
};

template <typename T>
class LockFreeStack
{
private:
	// Our stack structure is a linked list.
	// Update:
	// Now this node inherits from HazardNode,
	// and we use that base for deleting/reclaiming nodes safely.
	struct Node : public HazardNode
	{
		// Node data.
		T data;

		// Pointer to the next node.
		Node* next;

		// Constructor for convenience.
		Node(T d) : data(d), next(nullptr) {}

		// Thanks to polymorphism, the real type T is found and destroyed correctly.
		~Node() override = default; // T data is destroyed here.
	};

	// The head always points to the latest/top node in the linked list.
	// It is atomic because multiple threads may try to read or change it at the same time.
	std::atomic<Node*> head_{ nullptr };

	// Reference to the HP engine/domain so we can use it later inside pop.
	HazardPointerDomain& domain_;

public:
	// Constructor receives the domain and stores it for later use.
	LockFreeStack(HazardPointerDomain& d) : domain_(d) {}

	// Push operation.
	// Multiple threads may try to get and change head at the same time,
	// so we must manage it correctly.
	void push(T data)
	{
		// Create a new node in memory using the given data.
		Node* newNode = new Node(data);

		// Store the current head address.
		auto currnetHead = head_.load(std::memory_order_relaxed);

		// The next pointer of the new node must point to the current head.
		// Later when this new node becomes the head,
		// it must still know the address of the next node.
		// That next node is the old head before push,
		// so the chain connection stays correct.
		newNode->next = currnetHead;

		// Keep trying until we succeed.
		// memory_order_release:
		// on success, other threads can see the changes we made.
		// memory_order_relaxed:
		// on failure, we just read/update the new address without extra synchronization cost.
		while (!head_.compare_exchange_weak(currnetHead,
			newNode,
			std::memory_order_release,
			std::memory_order_relaxed))
		{
			// If we failed, it means another thread pushed or popped,
			// and our current head is no longer valid.
			// So we must update it and try again.
			// CAS does this automatically and puts the real current head into currnetHead.
			// So here we only need to be careful about the chain connection
			// and update it again.
			newNode->next = currnetHead;
		}
	}

	// Pop method.
	// This one is more sensitive.
	std::optional<T> pop(ThreadContext& ctx)
	{
		// Try to pop a node.
		while (true)
		{
			// We read the current head and also write it into the announcement board,
			// saying that we are working with this node.
			auto* oldHead = ctx.protect(head_);

			// The main power of optional is here.
			// When the stack is empty, we exit with a clear signal.
			if (oldHead == nullptr)
			{
				// Here we clearly understand the stack was empty,
				// without any confusion.
				return std::nullopt;
			}
			else
			{
				// Now that we are sure the head node we read is protected
				// by the hazard pointer,
				// we can safely read and store the address of the next node.
				Node* next_node = oldHead->next;

				// Now we try to pop the data,
				// but before that we must preserve the chain connection correctly.
				// The current head is old now,
				// so we need to update head so it no longer points to the node being removed.
				// Keep going until we successfully update head.
				// memory_order_acquire:
				// on success, the data written by the previous thread becomes visible to us.
				if (head_.compare_exchange_weak(oldHead,
					next_node,
					std::memory_order_acquire,
					std::memory_order_relaxed))
				{
					// We successfully updated head.
					// The old head now needs to be removed eventually.

					// First save/move its data.
					auto data = std::move(oldHead->data);

					// Tell others we are done with this node,
					// so if someone wants to delete it, they can check safely.
					ctx.clear_hp();

					// Give the responsibility of deletion and thread-safe reclamation
					// to the hazard pointer system.
					// The node goes into the current thread's trash bin.
					// Maybe another thread was still using this node,
					// or had stored it in its memory before we got here,
					// or even an ABA situation may be around.
					ctx.retire(oldHead);

					// Return the data and exit the function.
					return data;
				}
				else
				{
					// We failed and must try again.
					// It means another thread pushed or popped something.
					// The loop will automatically repeat.
				}
			}
		}
	}

	~LockFreeStack()
	{
		// At this point we assume no thread is using the stack anymore,
		// so we start deleting the remaining nodes.
		// Read the current head with acquire,
		// so changes made by previous threads become visible to us.
		auto current_head = head_.load(std::memory_order_acquire);

		// Continue while there are still nodes in the stack.
		while (current_head != nullptr)
		{
			// Move one node forward and save it.
			// Because if the current node gets deleted,
			// we lose access to the next node.
			auto next_node = current_head->next;

			// Now delete the current node.
			delete current_head;

			// Move to the next node and keep going until everything is done.
			current_head = next_node;
		}
	}
};

int main()
{
	// Global domain that manages garbage.
	HazardPointerDomain domain;

	// Our lock-free stack connected to the domain.
	LockFreeStack<int> stack(domain);

	// Stress test settings.
	// constexpr: these settings are fixed at compile time and do not change.
	constexpr int NUM_PRODUCERS = 8;
	constexpr int NUM_CONSUMERS = 8;
	constexpr int ITEMS_PER_PRODUCER = 200000;

	// Total number of items that are supposed to be produced.
	constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

	// An atomic counter to make sure no data gets lost.
	std::atomic<int> total_popped{ 0 };

	// High-precision timer for checking performance.
	auto start_time = std::chrono::high_resolution_clock::now();

	// Using jthread in C++20 for automatic thread management.
	// They automatically join at the end of the program.
	std::vector<std::jthread> producers;
	std::vector<std::jthread> consumers;

	// Producer threads.
	for (size_t i = 0; i < NUM_PRODUCERS; ++i)
	{
		// emplace_back:
		// Instead of first creating a thread and then adding it to the vector,
		// which has extra cost,
		// we give the construction instruction directly to the vector.
		// It builds the thread inside itself faster and with lower overhead,
		// and places it in the right location.
		producers.emplace_back([&]()
			{
				// We create data and push it.
				// Notice that the outer loop creates threads quickly,
				// and the inner loop runs separately inside each thread.
				// Very fast and parallel.
				for (size_t j = 0; j < ITEMS_PER_PRODUCER; ++j)
				{
					stack.push(j);
				}
			});
	}

	// Consumer threads.
	for (size_t i = 0; i < NUM_CONSUMERS; ++i)
	{
		consumers.emplace_back([&]()
			{
				// Local context for this consumer thread.
				ThreadContext ctx(domain);

				// Continue until all produced data has been popped.
				// Relaxed is enough because we only want to read the counter here;
				// the other synchronization details are not important for this check.
				while (total_popped.load(std::memory_order_relaxed) < TOTAL_ITEMS)
				{
					// Store the popped data.
					auto result = stack.pop(ctx);

					// If the stack had a node/data inside it.
					if (result.has_value())
					{
						// If pop was successful, increase the counter by one.
						total_popped.fetch_add(1, std::memory_order_relaxed);
					}
					else
					{
						// Stack is empty and there is no data right now.
						// Instead of making the CPU spin like crazy and keep asking:
						// "Did data arrive? Did data arrive?",
						// we give the program a tiny pause so it can sleep/wait a bit
						// until new data gets produced.
						// This is useful when consumers are faster than producers.
						std::this_thread::yield();
					}
				}
			});
	}

	// Synchronization.
	// Even though jthread joins automatically in its destructor,
	// we explicitly join here to make sure all threads are done
	// before calculating the final time.
	for (auto& t : producers) t.join();
	for (auto& t : consumers) t.join();

	// Calculate stress test time.
	auto end_time = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> elapsed = end_time - start_time;

	std::cout << "Stress Test Completed in " << elapsed.count() << " ms.\n";
	std::cout << "Total Items Popped: " << total_popped.load() << " / " << TOTAL_ITEMS << "\n";

	if (total_popped.load() == TOTAL_ITEMS) {
		std::cout << "Success: No Data Lost. Architecture is Solid.\n";
	}
	else {
		std::cout << "FATAL ERROR: Race Condition Detected!\n";
	}

	return 0;
}
