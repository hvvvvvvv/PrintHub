#include <BoundedQueue.hpp>

#include "TestSupport.h"

#include <memory>

namespace {

void testCapacityFifoAndReuse() {
    BoundedQueue<int> queue(2);
    require(queue.Empty() && queue.Size() == 0, "New queue state");
    require(queue.TryPush(11) && queue.TryPush(22), "Available capacity");
    require(!queue.TryPush(33) && queue.Size() == 2 && !queue.Empty(), "Full queue rejects without growing");
    require(queue.Pop() == 11 && queue.Size() == 1, "FIFO first item");
    require(queue.TryPush(44), "Pop must release capacity");
    require(queue.Pop() == 22 && queue.Pop() == 44, "Rejected item must not enter FIFO");
    require(queue.Empty() && queue.Size() == 0, "Drained state");
    require(queue.TryPush(55) && queue.TryPush(66), "Refill");
    queue.Clear();
    require(queue.Empty() && queue.Size() == 0, "Clear state");
    queue.Clear();
    require(queue.TryPush(77) && queue.Pop() == 77, "Reuse after repeated Clear");
}

void testZeroAndOneCapacity() {
    BoundedQueue<int> zero(0);
    require(!zero.TryPush(1) && !zero.TryPush(2) && zero.Empty() && zero.Size() == 0, "Zero-capacity queue");
    zero.Clear();
    require(!zero.TryPush(3), "Clear must not change zero capacity");
    BoundedQueue<int> one(1);
    require(one.TryPush(1) && !one.TryPush(2) && one.Pop() == 1, "Single-slot queue");
    require(one.TryPush(3) && one.Pop() == 3, "Single-slot reuse");
    // Pop on an empty queue has no defined contract and is deliberately not called.
}

void testMoveOnlyAndDestruction() {
    BoundedQueue<std::unique_ptr<int>> moveOnly(1);
    auto item = std::make_unique<int>(42);
    require(moveOnly.TryPush(std::move(item)) && !item, "Move-only insertion");
    auto popped = moveOnly.Pop();
    require(popped && *popped == 42 && moveOnly.Empty(), "Move-only extraction");

    BoundedQueue<std::shared_ptr<int>> queue(1);
    auto retained = std::make_shared<int>(1);
    const std::weak_ptr<int> retainedWeak = retained;
    require(queue.TryPush(std::move(retained)), "Owned resource insertion");
    auto rejected = std::make_shared<int>(2);
    const std::weak_ptr<int> rejectedWeak = rejected;
    // Keep the call in its own statement: the by-value argument is destroyed at
    // the end of the full expression, not necessarily inside TryPush().
    const bool rejectedAccepted = queue.TryPush(std::move(rejected));
    require(!rejectedAccepted, "Full queue must reject the item");
    require(rejectedWeak.expired(), "Rejected by-value item must be released");
    require(!retainedWeak.expired(), "Enqueued item released too soon");
    queue.Clear();
    require(retainedWeak.expired(), "Clear must release owned resources");
}

} // namespace

int main() {
    try {
        runTest("capacity, FIFO and reuse", testCapacityFifoAndReuse);
        runTest("zero and one capacity", testZeroAndOneCapacity);
        runTest("move-only values and destruction", testMoveOnlyAndDestruction);
    } catch (const std::exception& error) {
        std::cerr << "Bounded queue tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
